#pragma once

#include <QVarLengthArray>
#include <QtGlobal>
#include <vector>

#include "waveform/waveform.h"

/// Deck Lab: prebuilt geometry for the scrolling RGB waveform (pi4-perf).
///
/// Upstream builds the vertices of every visible pixel column on the CPU in
/// every frame and the OpenGL backend uploads them again from client memory.
/// On a Raspberry Pi 4 that is most of the remaining cost of a playing deck.
///
/// Upstream already snaps the scroll position to whole pixels and samples
/// column `pos` of a frame around visual frame
/// `(qRound(firstVisualFrame / increment) + pos) * increment`, so the content
/// of a column depends only on its global index
/// `k = qRound(firstVisualFrame / increment) + pos` (for a fixed zoom, rate,
/// size and waveform). This cache stores columns by `k` in fixed-size chunks
/// held in a small ring of slots. A frame only computes `k0` for its first
/// column and draws the cached chunks shifted by `-k0` pixels, which is
/// exact float arithmetic at a device pixel ratio of 1 (integers and halves).
///
/// Colour, height, the EQ and visual gains, the deck gain and the theme
/// colours are applied by the vertex shader from uniforms, so changing them
/// costs nothing. A vertex stores the raw 0..255 band maxima instead.
///
/// The cache rebuilds every chunk only when the waveform object, the visual
/// frames per pixel (zoom or tempo), the device pixel ratio, the width or the
/// stereo split option change. Chunks built while the waveform was still
/// being analysed are rebuilt once more data arrived.
///
/// This class has no OpenGL dependency, so its output can be tested and
/// compared against the upstream renderer without a GPU.
class WaveformColumnCache {
  public:
    static constexpr int kColumnsPerChunk = 128;
    /// The axis line occupies the first rectangle of the vertex array.
    static constexpr int kAxisVertices = 6;

    /// Interleaved vertex as uploaded to the GPU (12 bytes).
    struct Vertex {
        /// Column space x (logical pixels; screen x = x + offset), or screen x
        /// for the axis line.
        float x;
        /// Edge selector: y = halfBreadth + sign * height. -1 top, 0 centre
        /// line (one-sided rectangles), +1 bottom. For the axis line the
        /// height is 0.5.
        float sign;
        /// Raw per-column maxima (0..255), unscaled.
        quint8 low;
        quint8 mid;
        quint8 high;
        quint8 all;
    };
    static_assert(sizeof(Vertex) == 12, "Vertex must stay tightly packed");

    struct Range {
        int firstVertex;
        int vertexCount;
    };
    using Ranges = QVarLengthArray<Range, 4>;

    struct Stats {
        quint64 frames = 0;
        quint64 resets = 0;
        quint64 chunksBuilt = 0;
        quint64 columnsBuilt = 0;
        quint64 bytesDirty = 0;
    };

    /// Prepares the columns [firstColumn, firstColumn + pixelLength).
    /// `length` is the logical widget length (for the axis line).
    /// Returns false if nothing can be drawn.
    bool prepare(const ConstWaveformPointer& pWaveform,
            double visualIncrementPerPixel,
            float devicePixelRatio,
            int pixelLength,
            int length,
            bool splitStereo,
            bool slip,
            int firstColumn);

    /// Releases the waveform and every chunk (track unloaded).
    void clear();
    bool holdsWaveform() const {
        return !m_pWaveform.isNull();
    }

    /// CPU copy of the whole vertex buffer (axis line, then the slots).
    const std::vector<Vertex>& vertices() const {
        return m_vertices;
    }
    int vertexCount() const {
        return static_cast<int>(m_vertices.size());
    }
    int verticesPerColumn() const {
        return m_verticesPerColumn;
    }
    int slotCount() const {
        return m_slotCount;
    }
    int slotFirstVertex(int slot) const {
        return kAxisVertices + slot * kColumnsPerChunk * m_verticesPerColumn;
    }
    int slotVertexCount() const {
        return kColumnsPerChunk * m_verticesPerColumn;
    }

    /// True once after the vertex array was resized: the GPU buffer must be
    /// reallocated and fully uploaded.
    bool takeReallocated() {
        const bool value = m_reallocated;
        m_reallocated = false;
        return value;
    }
    bool isAxisDirty() const {
        return m_axisDirty;
    }
    bool isSlotDirty(int slot) const {
        return m_slotDirty[static_cast<std::size_t>(slot)] != 0;
    }
    /// Marks everything as uploaded.
    void clearDirty();

    /// Vertex ranges of the visible columns, in draw order (at most two,
    /// because consecutive chunks live in consecutive slots of the ring).
    const Ranges& drawRanges() const {
        return m_drawRanges;
    }
    int firstColumn() const {
        return m_firstColumn;
    }

    const Stats& stats() const {
        return m_stats;
    }
    void resetStats() {
        m_stats = Stats();
    }

  private:
    struct Slot {
        bool valid = false;
        /// Built from a waveform that was still being analysed and touches
        /// data at or beyond the completion at build time.
        bool provisional = false;
        int chunk = 0;
        int completionAtBuild = 0;
    };

    void reset(int pixelLength);
    int slotOf(int chunk) const;
    bool ensureChunk(int chunk, int completion);
    void buildChunk(int chunk, Slot* pSlot, int completion);
    void buildAxis(int length);

    ConstWaveformPointer m_pWaveform;
    int m_dataSize = 0;
    double m_visualIncrementPerPixel = 0.0;
    float m_devicePixelRatio = 0.f;
    int m_pixelLength = -1;
    int m_length = -1;
    bool m_splitStereo = false;
    bool m_slip = false;

    int m_verticesPerColumn = 6;
    int m_slotCount = 0;
    std::vector<Slot> m_slots;
    std::vector<char> m_slotDirty;
    std::vector<Vertex> m_vertices;
    bool m_reallocated = false;
    bool m_axisDirty = false;

    int m_firstColumn = 0;
    int m_lastFirstColumn = 0;
    int m_direction = 1;
    Ranges m_drawRanges;
    Stats m_stats;
};
