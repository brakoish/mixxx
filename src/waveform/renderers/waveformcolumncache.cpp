#include "waveform/renderers/waveformcolumncache.h"

#include <QtMath>
#include <algorithm>
#include <cmath>

#include "util/math.h"

namespace {

int floorDiv(int value, int divisor) {
    int quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

/// Visual frames per pixel compare equal within this relative tolerance.
/// Upstream recomputes the increment every frame from the play position
/// (last - first), which wobbles in the last few bits while playing; real
/// zoom and tempo changes are many orders of magnitude larger.
constexpr double kIncrementTolerance = 1e-9;

} // namespace

void WaveformColumnCache::clear() {
    m_pWaveform.reset();
    m_dataSize = 0;
    m_visualIncrementPerPixel = 0.0;
    m_pixelLength = -1;
    m_length = -1;
    for (auto& slot : m_slots) {
        slot.valid = false;
    }
    m_drawRanges.clear();
}

void WaveformColumnCache::reset(int pixelLength) {
    ++m_stats.resets;
    // Visible columns span at most ceil(pixelLength / C) + 1 chunks. One more
    // slot on each side keeps the chunk just behind and prefetches the chunk
    // just ahead without ever evicting a visible one.
    const int slotCount = (pixelLength + kColumnsPerChunk - 1) / kColumnsPerChunk + 3;
    const auto vertexCount = static_cast<std::size_t>(
            kAxisVertices + slotCount * kColumnsPerChunk * m_verticesPerColumn);
    if (slotCount != m_slotCount || vertexCount != m_vertices.size()) {
        m_slotCount = slotCount;
        m_slots.assign(static_cast<std::size_t>(slotCount), Slot());
        m_slotDirty.assign(static_cast<std::size_t>(slotCount), 0);
        m_vertices.assign(vertexCount, Vertex{0.f, 0.f, 0, 0, 0, 0});
        m_reallocated = true;
    } else {
        for (auto& slot : m_slots) {
            slot.valid = false;
        }
    }
    m_axisDirty = true;
    m_length = -1;
}

int WaveformColumnCache::slotOf(int chunk) const {
    const int slot = chunk % m_slotCount;
    return slot < 0 ? slot + m_slotCount : slot;
}

bool WaveformColumnCache::prepare(const ConstWaveformPointer& pWaveform,
        double visualIncrementPerPixel,
        float devicePixelRatio,
        int pixelLength,
        int length,
        bool splitStereo,
        bool slip,
        int firstColumn) {
    m_drawRanges.clear();
    if (!pWaveform || pixelLength <= 0 || visualIncrementPerPixel <= 0.0) {
        return false;
    }
    const int dataSize = pWaveform->getDataSize();
    if (dataSize <= 1) {
        return false;
    }
    ++m_stats.frames;

    const int verticesPerColumn = (splitStereo && !slip) ? 12 : 6;
    const bool sameIncrement = std::abs(visualIncrementPerPixel - m_visualIncrementPerPixel) <=
            kIncrementTolerance * m_visualIncrementPerPixel;
    if (pWaveform != m_pWaveform ||
            dataSize != m_dataSize ||
            !sameIncrement ||
            devicePixelRatio != m_devicePixelRatio ||
            pixelLength != m_pixelLength ||
            splitStereo != m_splitStereo ||
            slip != m_slip ||
            m_slotCount == 0) {
        m_pWaveform = pWaveform;
        m_dataSize = dataSize;
        m_visualIncrementPerPixel = visualIncrementPerPixel;
        m_devicePixelRatio = devicePixelRatio;
        m_pixelLength = pixelLength;
        m_splitStereo = splitStereo;
        m_slip = slip;
        m_verticesPerColumn = verticesPerColumn;
        reset(pixelLength);
        m_lastFirstColumn = firstColumn;
    }
    if (length != m_length) {
        buildAxis(length);
    }

    // Data written by a running analysis is final below the completion
    // index; chunks touching data at or beyond it are rebuilt as it grows.
    const int completion = pWaveform->getCompletion();

    m_firstColumn = firstColumn;
    if (firstColumn != m_lastFirstColumn) {
        m_direction = firstColumn > m_lastFirstColumn ? 1 : -1;
        m_lastFirstColumn = firstColumn;
    }

    const int lastColumn = firstColumn + pixelLength - 1;
    const int firstChunk = floorDiv(firstColumn, kColumnsPerChunk);
    const int lastChunk = floorDiv(lastColumn, kColumnsPerChunk);
    int built = 0;
    for (int chunk = firstChunk; chunk <= lastChunk; ++chunk) {
        if (ensureChunk(chunk, completion)) {
            ++built;
        }
        const int chunkStart = chunk * kColumnsPerChunk;
        const int start = std::max(firstColumn, chunkStart);
        const int end = std::min(lastColumn + 1, chunkStart + kColumnsPerChunk);
        const int first = slotFirstVertex(slotOf(chunk)) +
                (start - chunkStart) * m_verticesPerColumn;
        const int count = (end - start) * m_verticesPerColumn;
        if (!m_drawRanges.isEmpty() &&
                m_drawRanges.last().firstVertex + m_drawRanges.last().vertexCount ==
                        first) {
            m_drawRanges.last().vertexCount += count;
        } else {
            m_drawRanges.append(Range{first, count});
        }
    }
    // Prefetch at most one chunk per frame in the direction of motion, and
    // only if no visible chunk had to be built in this frame, so the cost of
    // a frame never exceeds the visible width by more than one chunk.
    if (built == 0) {
        ensureChunk(m_direction > 0 ? lastChunk + 1 : firstChunk - 1, completion);
    }
    return true;
}

bool WaveformColumnCache::ensureChunk(int chunk, int completion) {
    Slot& slot = m_slots[static_cast<std::size_t>(slotOf(chunk))];
    if (slot.valid && slot.chunk == chunk &&
            (!slot.provisional || slot.completionAtBuild == completion)) {
        return false;
    }
    buildChunk(chunk, &slot, completion);
    return true;
}

void WaveformColumnCache::buildChunk(int chunk, Slot* pSlot, int completion) {
    const int slotIndex = slotOf(chunk);
    const WaveformData* data = m_pWaveform->data();
    const int dataSize = m_dataSize;
    const double increment = m_visualIncrementPerPixel;
    // Same sampling as allshader::WaveformRendererRGB::preprocessInner().
    const double maxSamplingRange = increment / 2.0;
    const float invDevicePixelRatio = 1.f / m_devicePixelRatio;
    const float halfPixelSize = 0.5f / m_devicePixelRatio;
    const bool split = m_splitStereo && !m_slip;
    const bool splitChannels = m_splitStereo;
    const float bottomSign = m_slip ? 0.f : 1.f;

    Vertex* pWrite = m_vertices.data() + slotFirstVertex(slotIndex);
    const int chunkStart = chunk * kColumnsPerChunk;
    int maxIndexUsed = -1;
    for (int column = chunkStart; column < chunkStart + kColumnsPerChunk; ++column) {
        const double xVisualFrame = static_cast<double>(column) * increment;
        const int visualFrameStart = std::lround(xVisualFrame - maxSamplingRange);
        const int visualFrameStop = std::lround(xVisualFrame + maxSamplingRange);

        const int visualIndexStart = std::max(visualFrameStart * 2, 0);
        const int visualIndexStop =
                std::min(std::max(visualFrameStop, visualFrameStart + 1) * 2, dataSize - 1);
        maxIndexUsed = std::max(maxIndexUsed, visualIndexStop);

        uchar u8maxLow[2]{};
        uchar u8maxMid[2]{};
        uchar u8maxHigh[2]{};
        uchar u8maxAll[2]{};
        for (int chn = 0; chn < 2; chn++) {
            const int signalChn = splitChannels ? chn : 0;
            for (int i = visualIndexStart + chn; i < visualIndexStop + chn; i += 2) {
                const WaveformData& waveformData = data[i];
                u8maxLow[signalChn] = math_max(u8maxLow[signalChn], waveformData.filtered.low);
                u8maxMid[signalChn] = math_max(u8maxMid[signalChn], waveformData.filtered.mid);
                u8maxHigh[signalChn] = math_max(u8maxHigh[signalChn], waveformData.filtered.high);
                u8maxAll[signalChn] = math_max(u8maxAll[signalChn], waveformData.filtered.all);
            }
        }

        const float fpos = static_cast<float>(column) * invDevicePixelRatio;
        const float x1 = fpos - halfPixelSize;
        const float x2 = fpos + halfPixelSize;
        const int rectangles = split ? 2 : 1;
        for (int chn = 0; chn < rectangles; ++chn) {
            const quint8 low = u8maxLow[chn];
            const quint8 mid = u8maxMid[chn];
            const quint8 high = u8maxHigh[chn];
            const quint8 all = u8maxAll[chn];
            // Same corner order as RGBVertexUpdater::addRectangle(x1, y1, x2, y2).
            float sign1;
            float sign2;
            if (!splitChannels) {
                sign1 = -1.f;
                sign2 = bottomSign;
            } else {
                // One-sided: from the centre line up (left channel) or down
                // (right channel).
                sign1 = 0.f;
                sign2 = chn == 0 ? -1.f : 1.f;
            }
            *pWrite++ = Vertex{x1, sign1, low, mid, high, all};
            *pWrite++ = Vertex{x2, sign1, low, mid, high, all};
            *pWrite++ = Vertex{x1, sign2, low, mid, high, all};
            *pWrite++ = Vertex{x1, sign2, low, mid, high, all};
            *pWrite++ = Vertex{x2, sign2, low, mid, high, all};
            *pWrite++ = Vertex{x2, sign1, low, mid, high, all};
        }
    }

    pSlot->valid = true;
    pSlot->chunk = chunk;
    pSlot->completionAtBuild = completion;
    pSlot->provisional = completion >= 0 && maxIndexUsed >= completion;
    m_slotDirty[static_cast<std::size_t>(slotIndex)] = 1;
    ++m_stats.chunksBuilt;
    m_stats.columnsBuilt += kColumnsPerChunk;
    m_stats.bytesDirty += static_cast<quint64>(slotVertexCount()) * sizeof(Vertex);
}

void WaveformColumnCache::buildAxis(int length) {
    m_length = length;
    const float x1 = 0.f;
    const float x2 = static_cast<float>(length);
    const float sign1 = -1.f;
    // The slip renderer draws only the upper half of the axis line.
    const float sign2 = m_slip ? 0.f : 1.f;
    Vertex* pWrite = m_vertices.data();
    *pWrite++ = Vertex{x1, sign1, 0, 0, 0, 0};
    *pWrite++ = Vertex{x2, sign1, 0, 0, 0, 0};
    *pWrite++ = Vertex{x1, sign2, 0, 0, 0, 0};
    *pWrite++ = Vertex{x1, sign2, 0, 0, 0, 0};
    *pWrite++ = Vertex{x2, sign2, 0, 0, 0, 0};
    *pWrite++ = Vertex{x2, sign1, 0, 0, 0, 0};
    m_axisDirty = true;
}

void WaveformColumnCache::clearDirty() {
    m_axisDirty = false;
    std::fill(m_slotDirty.begin(), m_slotDirty.end(), 0);
}
