/*
  mosaicwidget.cpp

  This file is part of QMemstat, a Qt GUI analyzer for program memory.
  Copyright (C) 2016-2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com

  Initial Author: Andreas Hartmetz <andreas.hartmetz@kdab.com>
  Maintainer: Christoph Sterz <christoph.sterz@kdab.com>

  Licensees holding valid commercial KDAB QMemstat licenses may use this file in
  accordance with QMemstat Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mosaicwidget.h"

#include <cassert>
#include <limits>
#include <utility>

#include <linux/kernel-page-flags.h>

#include <QEvent>
#include <QMouseEvent>

using namespace std;


///////////////////////////////////////////////////////////////////////////////////////////////////

// from linux/Documentation/vm/pagemap.txt
static const uint pageFlagCount = 32;
static const char *pageFlagNames[pageFlagCount] = {
    // KPF_* flags from kernel-page-flags.h, documented in linux/Documentation/vm/pagemap.txt -
    // those flags are specifically meant to be stable user-space API
    "LOCKED",
    "ERROR",
    "REFERENCED",
    "UPTODATE",
    "DIRTY",
    "LRU",
    "ACTIVE",
    "SLAB",
    "WRITEBACK",
    "RECLAIM",  // 9 (10 for 1-based indexing)
    "BUDDY",
    "MMAP",
    "ANON",
    "SWAPCACHE",
    "SWAPBACKED",
    "COMPOUND_HEAD",
    "COMPOUND_TAIL",
    "HUGE",
    "UNEVICTABLE",
    "HWPOISON", // 19
    "NOPAGE",
    "KSM",
    "THP",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    // flags from /proc/<pid>/pagemap, also documented in linux/Documentation/vm/pagemap.txt -
    // we shift them around a bit to clearly group them together and away from the other group,
    // as documented in readPagemap() in pageinfo.cpp: 55-> 28 ; 61 -> 29 ; 62 -> 30; 63 -> 31
    "SOFT_DIRTY",
    "FILE_PAGE / SHARE_ANON", // 29
    "SWAPPED",
    "PRESENT"
};

static bool isFlagSet(uint32_t flags, uint testFlagShift)
{
    return flags & (1 << testFlagShift);
}

QString printablePageFlags(uint32_t flags)
{
    QString ret;
    for (uint i = 0; i < pageFlagCount; i++) {
        if (isFlagSet(flags, i)) {
            assert(pageFlagNames[i]);
            ret += pageFlagNames[i];
            ret += ", ";
        }
    }
    if (ret.length() >= 2) {
        ret.resize(ret.length() - 2);
    }
    return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

static const uint s_pixelsPerTile = 4;
static const uint s_columnCount = 512;

bool PageInfoReader::addData(const QByteArray &data)
{
    m_buffer += data;
    bool ret = false;
    // is not guaranteed that there is one or less dataset per chunk of data received, so keep looping
    while (true) {
        if (m_length < 0 && size_t(m_buffer.length()) >= sizeof(size_t)) {
            m_length = *reinterpret_cast<const uint64_t *>(m_buffer.constData());
        }
        if (m_length >= 0 && size_t(m_buffer.length()) >= m_length + sizeof(size_t)) {
            ret = true;
            m_mappedRegions.clear();

            const size_t endPos = m_length + sizeof(m_length);
            const char *buf = m_buffer.constData();

            for (size_t pos = sizeof(size_t); pos < endPos; ) {
                MappedRegion mr;
                mr.start = *reinterpret_cast<const uint64_t *>(buf + pos);
                pos += sizeof(uint64_t);
                mr.end = *reinterpret_cast<const uint64_t *>(buf + pos);
                pos += sizeof(uint64_t);

                const uint32_t backingFileLength = *reinterpret_cast<const uint32_t *>(buf + pos);
                pos += sizeof(uint32_t);
                mr.backingFile = std::string(reinterpret_cast<const char *>(buf + pos), backingFileLength);
                pos += (backingFileLength + sizeof(uint32_t) - 1) & ~0x3;

                const size_t arrayLength = (mr.end - mr.start) / PageInfo::pageSize;

                const uint32_t *array = reinterpret_cast<const uint32_t *>(buf + pos);
                mr.useCounts.assign(array, array + arrayLength);
                array += arrayLength;
                mr.combinedFlags.assign(array, array + arrayLength);
                pos += 2 * arrayLength * sizeof(uint32_t);

                m_mappedRegions.push_back(move(mr));
            }

            m_buffer.remove(0, endPos);
            m_length = -1;
        } else {
            break;
        }
    }
    return ret;
}

// bypass QImage API to save cycles; it does make a difference.
class Rgb32PixelAccess
{
public:
    Rgb32PixelAccess(int width, int height, uchar *buffer)
       : m_width(width),
         m_height(height),
         m_buffer(reinterpret_cast<quint32 *>(buffer))
    {}
    inline quint32 pixel(int x, int y) const { return m_buffer[y * m_width + x]; }
    inline void setPixel(int x, int y, quint32 value) { m_buffer[y * m_width + x] = value | (0xff << 24); }

private:
    const int m_width;
    const int m_height;
    quint32 *const m_buffer;
};

class ColorCache
{
public:
    ColorCache()
       : m_cachedColor(0)
    {
        for (uint i = 0; i < s_pixelsPerTile * s_pixelsPerTile; i++) {
            m_colorCache[i] = 0;
        }
    }

    void paintTile(Rgb32PixelAccess *img, uint x, uint y, uint tileSize, const QColor &color);

private:
    // QColor::darker() is fairly slow, so memoize the result
    void maybeUpdateColors(const QColor &color);

    uint m_cachedColor;
    uint m_colorCache[s_pixelsPerTile * s_pixelsPerTile];
};

void ColorCache::maybeUpdateColors(const QColor &color)
{
    if (color.rgb() == m_cachedColor) {
        return;
    }
    m_cachedColor = color.rgb();
    QColor c = color;
    for (uint i = 0; i < s_pixelsPerTile * s_pixelsPerTile; i++) {
        m_colorCache[i] = c.rgb();
        c = c.darker(115);
    }
}

void ColorCache::paintTile(Rgb32PixelAccess *img, uint x, uint y, uint tileSize, const QColor &color)
{
    maybeUpdateColors(color);
    const uint yStart = y * tileSize;
    const uint xEnd = (x + 1) * tileSize;
    const uint yEnd = (y + 1) * tileSize;
    uint colorIndex = 0;
    for (x = x * tileSize ; x < xEnd; x++) {
        for (y = yStart ; y < yEnd; y++) {
            img->setPixel(x, y, m_colorCache[colorIndex]);
        }
    }
}

MosaicWidget::MosaicWidget(uint pid)
   : m_pid(pid)
{
    qDebug() << "local process";
    m_updateIntervalWatch.start();
    // we're not usually *reaching* 50 milliseconds update interval... but trying doesn't hurt.
    m_updateTimer.setInterval(50);
    connect(&m_updateTimer, SIGNAL(timeout()), SLOT(localUpdateTimeout()));
    m_updateTimer.start();
    localUpdateTimeout();

    m_mosaicWidget.installEventFilter(this);
    setWidget(&m_mosaicWidget);
}

MosaicWidget::MosaicWidget(const QByteArray &host, uint port)
   : m_pid(0)
{
    qDebug() << "process on server:" << host << port;
    connect(&m_socket, SIGNAL(readyRead()), SLOT(networkDataAvailable()));
    connect(&m_socket, SIGNAL(error(QAbstractSocket::SocketError)), SLOT(socketError()));
    m_socket.connectToHost(QString::fromLatin1(host), port, QIODevice::ReadOnly);

    setWidget(&m_mosaicWidget);
}

void MosaicWidget::localUpdateTimeout()
{
    PageInfo pageInfo(m_pid);
    if (!pageInfo.mappedRegions().empty()) {
        updatePageInfo(pageInfo.mappedRegions());
    } else {
        emit showPageInfo(0, 0, QString());
        // HACK: not stopping the timer because clients expect to get regular updates, most importantly
        //       they expect that missing the first update is not critical
        // m_updateTimer.stop();
        return;
    }
}

void MosaicWidget::networkDataAvailable()
{
    if (m_pageInfoReader.addData(m_socket.readAll())) {
        updatePageInfo(m_pageInfoReader.m_mappedRegions);
    }
}

void MosaicWidget::socketError()
{
    emit serverConnectionBroke(m_regions.size());
}

void MosaicWidget::updatePageInfo(const vector<MappedRegion> &regions)
{
    //qint64 elapsed = m_updateIntervalWatch.restart();
    //qDebug() << " >> frame interval" << elapsed << "milliseconds";

    m_regions = regions;
    m_largeRegions.clear();

    if (regions.empty()) {
        m_img = QImage();
        m_mosaicWidget.setPixmap(QPixmap::fromImage(m_img));
        m_mosaicWidget.adjustSize();
        return;
    }
#ifndef NDEBUG
    for (const MappedRegion &mappedRegion : regions) {
        assert(mappedRegion.end >= mappedRegion.start); // == unfortunately happens sometimes
    }
#endif
    for (size_t i = 1; i < regions.size(); i++) {
        if (regions[i].start < regions[i - 1].end) {
            qDebug() << "ranges.." << QString("%1").arg(regions[i - 1].start, 0, 16)
                                   << QString("%1").arg(regions[i - 1].end, 0, 16)
                                   << QString("%1").arg(regions[i].start, 0, 16)
                                   << QString("%1").arg(regions[i].end, 0, 16);

        }
        assert(regions[i].start >= regions[i - 1].end);
    }

    //const quint64 totalRange = regions.back().end - regions.front().start;
    //qDebug() << "Address range covered (in pages) is" << totalRange / PageInfo::pageSize;

    quint64 mappedSpace = 0;
    for (const MappedRegion &r : regions) {
        mappedSpace += r.end - r.start;
    }
    //qDebug() << "Number of pages in mapped address space (VSZ) is" << mappedSpace / PageInfo::pageSize;

    // The difference between page count in mapped address space and page count in the "spanned" address
    // space can be HUGE, so we must figuratively insert some (...) in the graphical representation. Find
    // the large contiguous regions and thus the points to graphically separate them.
    // TODO implement a separator later, be it a line, spacing, labeling....
    vector<pair<quint64, quint64>> largeRegions;
    {
        pair<quint64, quint64> largeRegion = make_pair(regions.front().start, regions.front().end);
        static const quint64 maxAllowedGap = 64 * PageInfo::pageSize;
        for (const MappedRegion &r : regions) {
            if (r.start > largeRegion.second + maxAllowedGap) {
                largeRegions.push_back(largeRegion);
                largeRegion.first = r.start;
            }
            largeRegion.second = r.end;
        }
        largeRegions.push_back(largeRegion);
    }

#if 0
    // for performance tuning...
    qDebug() << "number of large regions in the address space is" << largeRegions.size();
    for (auto &r : largeRegions) {
        // hex output...
        qDebug() << "region" << QString("%1").arg(r.first, 0, 16) << QString("%1").arg(r.second, 0, 16);
    }
#endif

    static const uint tilesPerSeparator = 2;

    // determine size
    // separators between largeRegions
    uint rowCount = (largeRegions.size() - 1) * tilesPerSeparator;
    // space for tiles showing showing pages (corresponding to contents of largeRegions)
    for (pair<quint64, quint64> largeRegion : largeRegions) {
        rowCount += ((largeRegion.second - largeRegion.first) / PageInfo::pageSize + (s_columnCount - 1)) /
                    s_columnCount;
    }
    //qDebug() << "row count is" << rowCount << " largeRegion count is" << largeRegions.size();

    // paint!

    m_img = QImage(s_columnCount * s_pixelsPerTile, rowCount * s_pixelsPerTile, QImage::Format_RGB32);
    // Theoretically we need to get the stride of the image, but in practice it is equal to width,
    // especially with the power-of-2 widths we are using.
    Rgb32PixelAccess pixels(m_img.width(), m_img.height(), m_img.bits());
    // don't always construct QColors from enums - this would eat ~ 10% or so of frame time.
    const QColor colorWhite(Qt::white);
    const QColor colorGray(Qt::darkGray);
    const QColor colorMagenta(Qt::magenta);
    const QColor colorMagentaLight = QColor(Qt::magenta).lighter(150);
    const QColor colorYellow(Qt::yellow);
    const QColor colorCyan(Qt::blue);
    const QColor colorGreen(Qt::green);
    const QColor colorGreenDark(Qt::darkGreen);
    const QColor colorRedDark(Qt::darkRed);
    const QColor colorBlack(Qt::black);
    // cache results of QColor::darken()
    ColorCache cc;

    uint row = 0;
    size_t iMappedRegion = 0;
    for (pair<quint64, quint64> largeRegion : largeRegions) {
        uint column = 0;
        assert(iMappedRegion < regions.size());
        const MappedRegion *region = &regions[iMappedRegion];

        m_largeRegions.push_back(make_pair(row, region->start));

        for ( ;region->end <= largeRegion.second; region = &regions[iMappedRegion]) {
            assert(iMappedRegion < regions.size());
            assert(region->end >= region->start);

            size_t iPage = 0;
            while (iPage < region->useCounts.size()) {
                const size_t endColumn = qMin(column + region->useCounts.size() - iPage, size_t(s_columnCount));
                //qDebug() << "painting region" << column << endColumn;
                for ( ; column < endColumn; column++, iPage++) {
                    //qDebug() << "magenta" << column << row;
                    QColor color = colorWhite;
                    if (!(region->combinedFlags[iPage] & (1 << 31))) { // TODO no magic numbers - checking if "present" flag clear here
                        color = colorGray;
                    } else if ((region->combinedFlags[iPage] & (1 << KPF_MMAP)) &&
                               !(region->combinedFlags[iPage] & (1 << KPF_ANON))) {
                        color = region->useCounts[iPage] > 1 ? colorGreen : colorGreenDark;
                    } else if (region->combinedFlags[iPage] & (1 << KPF_THP)) {
                        // THP implies use count 1; the kernel wrongly reports use count 0 in this case
                        color = colorMagentaLight;
                    } else if (region->useCounts[iPage] == 1) {
                        color = colorMagenta;
                    } else if (region->useCounts[iPage] > 1) {
                        color = colorYellow;
                    } else if (region->combinedFlags[iPage] & (1 << KPF_NOPAGE)) {
                        color = colorRedDark;
                    } else {
                        // qDebug() << "white page has use count" << region->useCounts[iPage] << "and flags"
                        //          << printablePageFlags(region->combinedFlags[iPage]);
                    }
                    cc.paintTile(&pixels, column, row, s_pixelsPerTile, color);
                }
                if (column == s_columnCount) {
                    column = 0;
                    row++;
                }
            }
            assert(region->start + iPage * PageInfo::pageSize == region->end);

            // fill tiles up to either next MappedRegion or (if at end of current largeRegion) end of row

            // why increase it here? a) convenience, otherwise there'd be a lot of "iMappedRegion + 1" below
            // b) correctness, we need to fill up the current row if we're at end of region. this could be
            //    done differently, but i don't think it's worth it if you think about how to do it.
            iMappedRegion++;

            assert(column <= s_columnCount);
            size_t gapPages = column ? (s_columnCount - column) : 0;
            if (iMappedRegion < regions.size() && regions[iMappedRegion].start < largeRegion.second) {
                //qDebug() << "doing it..." << QString("%1").arg(region->end, 0, 16)
                //                          << QString("%1").arg(regions[iMappedRegion].start, 0, 16);
                assert(regions[iMappedRegion].start >= region->end);
                // region still points to previous MappedRegion...
                gapPages = (regions[iMappedRegion].start - region->end) / PageInfo::pageSize;
            }
            assert(gapPages < s_columnCount);
            while (gapPages) {
                const size_t endColumn = qMin(size_t(s_columnCount), column + gapPages);
                gapPages -= (endColumn - column);
                //qDebug() << "painting gap" << column << endColumn << gapPages;
                for ( ; column < endColumn; column++) {
                    cc.paintTile(&pixels, column, row, s_pixelsPerTile, colorCyan);
                }
                if (column == s_columnCount) {
                    column = 0;
                    row++;
                }
            }

            assert(region->end <= largeRegion.second);
            // loop control snippet "region = &regions[iMappedRegion]" would access out of bounds otherwise
            if (iMappedRegion >= regions.size()) {
                break;
            }
        }
        assert(column == 0);
        // draw separator line; we avoid a line after the last largeRegion via the "&& y < rowCount"
        // condition and decreasing rowCount by the height (thickness) of a line.
        for (uint y = row; y < row + tilesPerSeparator && y < rowCount; y++) {
            for (uint x = 0 ; x < s_columnCount; x++) {
                cc.paintTile(&pixels, x, y, s_pixelsPerTile, colorBlack);
            }
        }
        row += tilesPerSeparator;
    }

    m_mosaicWidget.setPixmap(QPixmap::fromImage(m_img));
    m_mosaicWidget.adjustSize();
}

void MosaicWidget::printPageFlagsAtPos(const QPoint &widgetPos)
{
    printPageFlagsAtAddr(addressAtPos(widgetPos));
}

quint64 MosaicWidget::addressAtPos(const QPoint &widgetPos)
{
    // widgetPos can be outside of the widget when the mouse button goes down inside the widget rect,
    // the with button still down is moved outside the widget. Like a drag, but we don't implement DnD.
    quint32 row = qMax(0, widgetPos.y()) / s_pixelsPerTile;
    quint32 column = qBound(0, widgetPos.x() / int(s_pixelsPerTile), int(s_columnCount));

    auto lIt = upper_bound(m_largeRegions.begin(), m_largeRegions.end(), row,
                           [](quint32 lhs, const pair<quint32, quint64> &rhs)
                               { return lhs < rhs.first; });
    if (lIt == m_largeRegions.begin()) {
        // qDebug() << "out of range (row too small)";
        return 0;
    }
    --lIt; // now lIt is at the next less or equal element
           // (unless row > last row, which should not trip up callers)

    return lIt->second + ((row - lIt->first) * s_columnCount + column) * PageInfo::pageSize;
}

void MosaicWidget::printPageFlagsAtAddr(quint64 addr)
{
    if (!addr) {
        return;
    }

    const auto rIt = upper_bound(m_regions.begin(), m_regions.end(), addr,
                                 [](quint64 lhs, const MappedRegion &rhs)
                                     { return lhs < rhs.end; });
    if (rIt == m_regions.end()) {
        // qDebug() << "out of range (addr/row/column too large)";
        return;
    }
    if (rIt->start > addr) {
        Q_ASSERT(rIt != m_regions.begin()); // this can only happen when input data is inconsistent
        //qDebug() << QString("%1").arg(addr, 0, 16) // hex format
        //         << "in a gap between"
        //         << QString("%1").arg(rIt->start, 0, 16) << "and"
        //         << QString("%1").arg((rIt - 1)->end, 0, 16) << "!";
        return;
    }

    const size_t index = (addr - rIt->start) / PageInfo::pageSize;

    emit showFlags(rIt->combinedFlags[index]);
    emit showPageInfo(addr, rIt->useCounts[index], QString::fromStdString(rIt->backingFile));
}

bool MosaicWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->buttons() & Qt::LeftButton) {
            printPageFlagsAtPos(me->pos());
            return true;
        }
    }
    return QScrollArea::eventFilter(obj, event);
}
