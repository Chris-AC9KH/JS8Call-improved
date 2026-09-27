/**
 * @file CPlotter.cpp
 * @brief GPU implementation of the waterfall plotter.
 */

#include "CPlotter.h"
#include "DriftingDateTime.h"
#include "JS8_Include/commons.h"
#include "JS8_Mode/JS8Submode.h"

#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QToolTip>
#include <QVector4D>
#include <QWheelEvent>

#include <concepts>
#include <cstring>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <utility>

#include "moc_CPlotter.cpp"

Q_LOGGING_CATEGORY(plotter_js8, "plotter.js8", QtWarningMsg)

// Constants
namespace {

// Debounce interval, in milliseconds; adjust to taste.
constexpr auto DEBOUNCE_INTERVAL = 100;

// Vertical divisions in the spectrum display.
constexpr std::size_t VERT_DIVS = 7;

// FFT bin width, as with NSPS, a constant; see the JT9 documentation
// for the reasoning behind the values used here, but in short, since
// NSPS is always 6912, 1500 for nsps2 and 2048 for nfft3 are optimal.
constexpr float FFT_BIN_WIDTH = 1500.0 / 2048.0;

// 30 meter band
constexpr float BAND_30M_START = 10.13f;
constexpr float BAND_30M_END = 10.15f;

// The WSPR range starts at 10.1401 MHz and runs for 200 Hz.
constexpr float WSPR_START = 10.1401f;
constexpr int WSPR_RANGE = 200;

// Band colors, always drawn with a 3-pixel pen.
constexpr auto BAND_EDGE = QColor{149, 165, 166}; // Gray
constexpr auto BAND_GOOD = QColor{46, 204, 113};  // Green
constexpr auto BAND_WARN = QColor{241, 196, 15};  // Yellow
constexpr auto BAND_WSPR = QColor{230, 126, 34};  // Orange

// Uniform buffer layouts, std140.
constexpr int QUAD_UBUF_SIZE = 32;
constexpr int LINE_UBUF_SIZE = 16;
} // namespace

// Local Utilities
namespace {
template <std::floating_point T> constexpr auto fractionalPart(T const v) {
    T integralPart;
    return std::modf(v, &integralPart);
}

auto freqPerDiv(float const fSpan) {
    if (fSpan > 2500) return 500;
    if (fSpan > 1000) return 200;
    if (fSpan > 500) return 100;
    if (fSpan > 250) return 50;
    if (fSpan > 100) return 20;
    return 10;
}

// Loads a shader baked by qt_add_shaders() / qsb.
QShader loadShader(QString const &resourcePath) {
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "CPlotter: failed to open shader" << resourcePath;
        return QShader();
    }
    return QShader::fromSerialized(f.readAll());
}

// Maps a point already in "logical" NDC space (-1..1, y-down as in the
// rest of this file) into the backend's actual clip space. Backends
// differ in framebuffer Y orientation
QPointF toClipSpace(QRhi const *rhi, float ndcX, float ndcY) {
    QVector4D const v =
        rhi->clipSpaceCorrMatrix() * QVector4D(ndcX, -ndcY, 0.0f, 1.0f);
    // Note the -ndcY: our layout math elsewhere treats +Y as "down the
    // widget" (matching QPainter/QImage convention), so we flip once here
    // before backend correction, rather than threading a flip through
    // every call site.
    return QPointF(v.x(), v.y());
}
} // namespace

// Helper method for resource update batches
QRhiResourceUpdateBatch *CPlotter::pendingUpdateBatch() {
    if (!m_rhi)
        return nullptr;

    if (!m_pendingUpdates)
        m_pendingUpdates = m_rhi->nextResourceUpdateBatch();
    return m_pendingUpdates;
}

// Construction / sizing
CPlotter::CPlotter(QWidget *parent)
    : QRhiWidget{parent},
      // Initialize primitive layouts in strict header declaration sequence
      m_percent2D{0},
      m_binsPerPixel{2},
      m_waterfallAvg{1},
      m_lastMouseX{-1},
      m_line{std::numeric_limits<int>::max()},
      m_startFreq{0},
      m_freq{0},
      m_w{0},
      m_h1{0},
      m_h2{0},
      m_filterEnabled{false},
      m_freqPerPixel{2 * FFT_BIN_WIDTH},
      m_scaler1D{m_waterfallAvg, m_binsPerPixel},
      m_scaler2D{m_h2},

      m_replotTimer{nullptr},
      m_resizeTimer{nullptr}
{
    // Allocate the heap timers
    m_replotTimer = new QTimer(this);
    m_resizeTimer = new QTimer(this);

    m_replotTimer->setSingleShot(true);
    m_resizeTimer->setSingleShot(true);
    m_replotTimer->setInterval(DEBOUNCE_INTERVAL);
    m_resizeTimer->setInterval(DEBOUNCE_INTERVAL);

    // Signal slot connections
    connect(m_replotTimer, &QTimer::timeout, this, &CPlotter::replot);
    connect(m_resizeTimer, &QTimer::timeout, this, &CPlotter::resize);

    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    // Initialize local structural parameters
    for (int i = 0; i < 2; ++i) {
        m_dialQuad[i].structureDirty = true;
        m_dialQuad[i].contentDirty = false;
        m_dialQuad[i].opacity = 1.0f;

        m_filterQuad[i].structureDirty = true;
        m_filterQuad[i].contentDirty = false;
        m_filterQuad[i].opacity = 1.0f;
    }

    m_scaleQuad.structureDirty = true;
    m_waterfallQuad.structureDirty = true;
    m_overlayQuad.structureDirty = true;
}

CPlotter::~CPlotter() = default;

QSize CPlotter::minimumSizeHint() const { return QSize(50, 50); }

QSize CPlotter::sizeHint() const { return QSize(180, 180); }

// QRhiWidget entry points

// Called whenever the widget needs (re)establishing against its QRhi --
// first show, and whenever the backend/device changes. NOT called on
// every resize; per-size GPU resource (re)allocation is handled lazily
// in ensureGpuState(), invoked from render(), because that's the only
// place we're guaranteed a live QRhi and an open command buffer to
// queue the initial uploads on.
void CPlotter::initialize(QRhiCommandBuffer *) {
    bool const backendChanged = (m_rhi != rhi());
    m_rhi = rhi();

    static constexpr float quad[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 1.0f,
    };

    if (backendChanged || !m_quadVBuf) {
        m_quadVBuf.reset(m_rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
        m_quadVBuf->create();

        m_pipelineDummyLayoutBuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_rhi->ubufAligned(QUAD_UBUF_SIZE)));
        m_pipelineDummyLayoutBuf->create();

        m_pipelineDummyLayoutTex.reset(m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
        m_pipelineDummyLayoutTex->create();

        m_sampler.reset(m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None, QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();

        m_quadVertShader = loadShader(":/shaders/texquad.vert.qsb");
        m_quadFragShader = loadShader(":/shaders/texquad.frag.qsb");
        m_lineVertShader = loadShader(":/shaders/specline.vert.qsb");
        m_lineFragShader = loadShader(":/shaders/specline.frag.qsb");

        auto *u = m_rhi->nextResourceUpdateBatch();
        u->uploadStaticBuffer(m_quadVBuf.get(), quad);

        m_waterfallQuad.texture.reset();
        m_waterfallQuad.uniformBuf.reset();
        m_waterfallQuad.srb.reset();
        m_waterfallQuad.structureDirty = true;
        ensureQuadGpuState(m_waterfallQuad, QSize(1, 1), u);

        m_pendingUpdates = u;
    }

    // Rebuild pipelines any time initialize() runs and the render pass
    // descriptor we last built against no longer matches the current
    // one. Qt only calls initialize() when something like this needs
    // re-establishing, so don't gate this on a "first time ever" flag.
    QRhiRenderPassDescriptor *rpDesc = renderTarget()->renderPassDescriptor();
    bool const rpChanged =
        !m_quadPipeline || m_quadPipeline->renderPassDescriptor() != rpDesc;

    if (rpChanged) {
        m_lineSrb.reset(m_rhi->newShaderResourceBindings());
        m_lineSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_pipelineDummyLayoutBuf.get()),
        });
        m_lineSrb->create();

        m_linePipeline.reset(m_rhi->newGraphicsPipeline());
        m_linePipeline->setShaderStages({{QRhiShaderStage::Vertex, m_lineVertShader}, {QRhiShaderStage::Fragment, m_lineFragShader}});
        QRhiVertexInputLayout lineLayout;
        lineLayout.setBindings({{2 * sizeof(float)}});
        lineLayout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
        m_linePipeline->setVertexInputLayout(lineLayout);
        m_linePipeline->setTopology(QRhiGraphicsPipeline::LineStrip);
        m_linePipeline->setShaderResourceBindings(m_lineSrb.get());
        m_linePipeline->setRenderPassDescriptor(rpDesc);
        if (!m_linePipeline->create())
            qFatal("CPlotter: m_linePipeline->create() failed");

        m_quadPipelineLayoutSrb.reset(m_rhi->newShaderResourceBindings());
        m_quadPipelineLayoutSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_pipelineDummyLayoutBuf.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, m_pipelineDummyLayoutTex.get(), m_sampler.get()),
        });
        m_quadPipelineLayoutSrb->create();

        m_quadPipeline.reset(m_rhi->newGraphicsPipeline());
        m_quadPipeline->setShaderStages({{QRhiShaderStage::Vertex, m_quadVertShader}, {QRhiShaderStage::Fragment, m_quadFragShader}});
        QRhiVertexInputLayout quadLayout;
        quadLayout.setBindings({{4 * sizeof(float)}});
        quadLayout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float2, 0},
            {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)}});
        m_quadPipeline->setVertexInputLayout(quadLayout);
        m_quadPipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        {
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            m_quadPipeline->setTargetBlends({blend});
        }
        m_quadPipeline->setShaderResourceBindings(m_quadPipelineLayoutSrb.get());
        m_quadPipeline->setRenderPassDescriptor(rpDesc);
        if (!m_quadPipeline->create())
            qFatal("CPlotter: m_quadPipeline->create() failed");
    }

    if (!m_lineVBuf) {
        m_lineVBuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, 4096 * 2 * sizeof(float)));
        m_lineVBuf->create();
    }
    if (!m_lineUniformBuf) {
        m_lineUniformBuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, m_rhi->ubufAligned(LINE_UBUF_SIZE)));
        m_lineUniformBuf->create();
    }
    
    m_lineSrb.reset(m_rhi->newShaderResourceBindings());
    m_lineSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_lineUniformBuf.get()),
    });
    m_lineSrb->create();
}

void CPlotter::render(QRhiCommandBuffer *cb) {
    if (!m_rhi || !cb || !renderTarget())
        return;

    // Ingest resource update batches
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    if (m_pendingUpdates) {
        u->merge(m_pendingUpdates);
        m_pendingUpdates->release();
        m_pendingUpdates = nullptr;
    }
    
    ensureGpuState(u);

    QColor const clearColor = Qt::black;
    QRhiRenderTarget *rt = renderTarget();

    cb->beginPass(rt, clearColor, {1.0f, 0}, u);
    cb->setViewport(QRhiViewport(0, 0, rt->pixelSize().width(), rt->pixelSize().height()));

    if (m_waterfallQuad.texture && m_waterfallQuad.srb && !m_waterfallQuad.structureDirty) {

        // Draw only the core baseline window quads
        if (m_scaleQuad.texture && m_scaleQuad.srb) {
            renderTexQuad(cb, m_scaleQuad);
        }

        renderTexQuad(cb, m_waterfallQuad);

        if (m_overlayQuad.texture && m_overlayQuad.srb) {
            renderTexQuad(cb, m_overlayQuad);
        }

        // Render the green/cyan spectrum line strip trace
        if (m_lineVertexCount >= 2 && m_linePipeline && m_lineSrb) {
            cb->setGraphicsPipeline(m_linePipeline.get());
            cb->setShaderResources(m_lineSrb.get());
            QRhiCommandBuffer::VertexInput const vbufBinding(m_lineVBuf.get(), 0);
            cb->setVertexInput(0, 1, &vbufBinding);
            cb->draw(m_lineVertexCount);
        }

        // wire up the gpu dials pass
        if (m_dialQuad[0].texture && m_dialQuad[0].srb) {
            renderTexQuad(cb, m_dialQuad[0]);
        }
        if (m_lastMouseX >= 0 && m_dialQuad[1].texture && m_dialQuad[1].srb) {
            renderTexQuad(cb, m_dialQuad[1]);
        }

        // wire up the passband filter shading layers
        if (m_filterEnabled && m_filterWidth > 0) {
            if (m_filterQuad[0].texture && m_filterQuad[0].srb) {
                renderTexQuad(cb, m_filterQuad[0]);
            }
            if (m_filterQuad[1].texture && m_filterQuad[1].srb) {
                renderTexQuad(cb, m_filterQuad[1]);
            }
        }
    }
    cb->endPass();
}

// GPU resource management
// Top of every render(): walk each quad and (re)create/upload anything
// that changed since last frame.
void CPlotter::ensureGpuState(QRhiResourceUpdateBatch *u) {
    if (m_pendingUpdates) {
        u->merge(m_pendingUpdates);
        m_pendingUpdates->release();
        m_pendingUpdates = nullptr;
    }

    auto toNdcX = [this](int x) -> float {
        return 2.0f * (float(x) / std::max(1, m_w)) - 1.0f;
    };
    float const totalH = float(size().height());

    // Forcing the left clip position to update on every frame ensures that the
    // red and white cursors follow mouse and slider inputs
    for (int i = 0; i < 2; ++i) {
        if (!m_dialQuad[i].image.isNull()) {
            int x = (i == 0) ? xFromFreq(m_freq) : std::max(0, m_lastMouseX);
            int w = m_dialQuad[i].image.width();
            int h = m_dialQuad[i].image.height();
            
            float ndcLeft  = toNdcX(x);
            float ndcWidth = 2.0f * (float(w) / std::max(1, m_w));
            
            float ndcTop    = m_waterfallQuad.ndcRect.top();
            float ndcHeight = m_waterfallQuad.ndcRect.height();

            m_dialQuad[i].ndcRect = QRectF(ndcLeft, ndcTop, ndcWidth, ndcHeight);
            
            m_dialQuad[i].structureDirty = true;
        }
    }

    ensureQuadGpuState(m_waterfallQuad, QSize(m_w, m_h1), u);
    ensureQuadGpuState(m_overlayQuad, QSize(m_w, m_h2), u);
    ensureQuadGpuState(m_scaleQuad, QSize(m_w, 30), u);
    
    if (!m_dialQuad[0].image.isNull()) ensureQuadGpuState(m_dialQuad[0], m_dialQuad[0].image.size(), u);
    if (!m_dialQuad[1].image.isNull()) ensureQuadGpuState(m_dialQuad[1], m_dialQuad[1].image.size(), u);
    
    // Connect the passband filter shading quads into the hardware pipeline allocation loop
    if (m_filterEnabled && m_filterWidth > 0) {
        if (!m_filterQuad[0].image.isNull()) ensureQuadGpuState(m_filterQuad[0], m_filterQuad[0].image.size(), u);
        if (!m_filterQuad[1].image.isNull()) ensureQuadGpuState(m_filterQuad[1], m_filterQuad[1].image.size(), u);
    }
}

void CPlotter::ensureQuadGpuState(TexQuad &quad, QSize pixelSize,
                                  QRhiResourceUpdateBatch *u) {
    if (pixelSize.isEmpty())
        return;

    bool const needsTexture =
        !quad.texture || quad.texture->pixelSize() != pixelSize;

    if (quad.structureDirty || needsTexture) {
        quad.texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, pixelSize));
        if (!quad.texture->create())
            qFatal("CPlotter: quad texture create() failed for %dx%d",
                   pixelSize.width(), pixelSize.height());

        if (quad.image.size() != pixelSize) {
            quad.image = QImage(pixelSize, QImage::Format_RGBA8888);
            quad.image.fill(Qt::transparent);
        }

        if (!quad.uniformBuf) {
            quad.uniformBuf.reset(m_rhi->newBuffer(
                QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                m_rhi->ubufAligned(QUAD_UBUF_SIZE)));
            if (!quad.uniformBuf->create())
                qFatal("CPlotter: quad uniform buffer create() failed");
        }

        quad.srb.reset(m_rhi->newShaderResourceBindings());
        quad.srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage |
                    QRhiShaderResourceBinding::FragmentStage,
                quad.uniformBuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage,
                quad.texture.get(), m_sampler.get()),
        });
        if (!quad.srb->create())
            qFatal("CPlotter: quad SRB create() failed");

        quad.structureDirty = false;
        quad.contentDirty = true;
    }

    if (quad.contentDirty && !quad.image.isNull()) {
        // DIAGNOSTIC TRACE: Track which textures are actually uploading rows
        QString qName = "Unknown";
        if (&quad == &m_waterfallQuad) qName = "Waterfall";
        else if (&quad == &m_scaleQuad) qName = "ScaleBar";
        else if (&quad == &m_overlayQuad) qName = "SpectrumBackground";

        qCDebug(plotter_js8) << "Texture Upload Triggered for [" << qName << "]:"
                             << "Image Size=" << quad.image.size()
                             << "Texture Target Size=" << quad.texture->pixelSize();

        QRhiTextureSubresourceUploadDescription sub(
            quad.image.constBits(),
            static_cast<qsizetype>(quad.image.sizeInBytes()));
        QRhiTextureUploadEntry const entry(0, 0, sub);
        QRhiTextureUploadDescription desc({entry});
        u->uploadTexture(quad.texture.get(), desc);
        quad.contentDirty = false;
    }

    auto const topLeft = toClipSpace(m_rhi, quad.ndcRect.left(), quad.ndcRect.top());
    auto const size = QPointF(quad.ndcRect.width(), quad.ndcRect.height());

    QString quadName = "Unknown";
    if (&quad == &m_waterfallQuad) quadName = "Waterfall";
    else if (&quad == &m_scaleQuad) quadName = "ScaleBar";
    else if (&quad == &m_overlayQuad) quadName = "SpectrumBackground";

    qCDebug(plotter_js8) << "Uniform Packing [" << quadName << "]:"
                         << "Raw ndcRect=" << quad.ndcRect
                         << "Computed Clip topLeft=" << topLeft
                         << "Computed Clip size=" << size;

    float const params[8] = {
        float(topLeft.x()),
        -float(topLeft.y()),
        float(size.x()),
        float(size.y()),
        quad.opacity,
        &quad == &m_waterfallQuad
            ? float(m_waterfallRowOffset) / std::max(1, m_h1)
            : 0.0f,
        0.0f,
        0.0f,
    };

    qCDebug(plotter_js8) << "Uniform Array Float Bytes for [" << quadName << "]:"
                         << "x=" << params[0] << "y=" << params[1]
                         << "z=" << params[2] << "w=" << params[3];
    u->updateDynamicBuffer(quad.uniformBuf.get(), 0, sizeof(params), params);
}

void CPlotter::renderTexQuad(QRhiCommandBuffer *cb, TexQuad &quad) {
    if (!quad.texture || !quad.srb || quad.structureDirty || !quad.srb.get())
        return;

    if (!m_quadPipeline || !cb)
        return;

    cb->setGraphicsPipeline(m_quadPipeline.get());
    cb->setShaderResources(quad.srb.get());
    
    QRhiCommandBuffer::VertexInput const vbufBinding(m_quadVBuf.get(), 0);
    cb->setVertexInput(0, 1, &vbufBinding);
    cb->draw(4);
}

// Data-driven drawing (hot paths)
void CPlotter::resizeEvent(QResizeEvent *) { m_resizeTimer->start(); }

void CPlotter::drawLine(QString const &text) {

    QImage rowImg(m_w, 1, QImage::Format_RGBA8888);
    rowImg.fill(Qt::green);

    {
        QPainter p(&rowImg);
        p.setPen(Qt::white);
        p.drawText(5, 0, text);
    }

    m_text = text;
    m_line = rowImg.height() * 12;
    m_replot.push_front(m_text);

    std::vector<uint32_t> row(m_w);
    std::memcpy(row.data(), rowImg.constBits(), m_w * sizeof(uint32_t));

    QRhiResourceUpdateBatch* u = m_rhi ? pendingUpdateBatch() : nullptr;

    if (u) {
        uploadWaterfallRow(row, u);
    }

    update();
}

void CPlotter::drawData(WF::SWide swide, WF::State const state) {
    m_flatten(swide.data(), m_w);

    m_rowScratch.resize(m_w);
    for (auto x = 0; x < m_w; ++x) {
        // Fix color channel mapping from AARRGGBB to standard hardware RGBA
        QColor col = m_colors[m_scaler1D(swide[x])];
        m_rowScratch[x] = (uint32_t(col.red())   << 0)  |
                          (uint32_t(col.green()) << 8)  |
                          (uint32_t(col.blue())  << 16) |
                          (uint32_t(255)         << 24);
    }

    if (--m_line == 0) {
        m_line = std::numeric_limits<int>::max();
    }

    QRhiResourceUpdateBatch* u = m_rhi ? pendingUpdateBatch() : nullptr;

    if (u) {
        uploadWaterfallRow(m_rowScratch, u);
    }

    if (shouldDrawSpectrum(state)) {
        auto const addPoint = [this](int const x, float const y) {
            m_points.emplace_back(x, m_scaler2D(y));
        };

        auto const addPoints = [this, &addPoint](auto const begin, auto const value) {
            auto const start = begin + static_cast<std::size_t>(m_startFreq / FFT_BIN_WIDTH + 0.5f);
            for (auto x = 0; x < m_w; ++x) {
                auto const first = start + x * m_binsPerPixel;
                addPoint(x, value(std::reduce(first, first + m_binsPerPixel) / m_binsPerPixel));
            }
        };

        m_points.clear();
        m_points.reserve(m_w);

        switch (m_spectrum) {
            case Spectrum::Current: {
                m_lineColor = Qt::green;
                auto const min = *std::min_element(swide.begin(), swide.begin() + m_w);
                for (auto x = 0; x < m_w; ++x) addPoint(x, swide[x] - min);
            } break;
            case Spectrum::Cumulative: {
                m_lineColor = Qt::cyan;
                addPoints(std::begin(specData.savg), [](auto const value) {
                    return 30.0f + 10.0f * std::log10(value);
                });
            } break;
            case Spectrum::LinearAvg: {
                m_lineColor = Qt::yellow;
                addPoints(std::begin(specData.slin), [](auto const value) { return value; });
            } break;
        }

        m_points.erase(m_rdp(m_points), m_points.end());

        if (u) {
            buildSpectrumGeometry(u);
        }
    } else {
        m_lineVertexCount = 0;
    }

    m_replot.push_front(std::move(swide));

    // Request an active pipeline update pass
    update();
}

// Uploads one row of RGBA8 pixels into the waterfall ring-buffer texture
// at the current write position, then advances the ring pointer
void CPlotter::uploadWaterfallRow(std::vector<uint32_t> const &rgba,
                                  QRhiResourceUpdateBatch *u) {
    if (!m_waterfallQuad.texture || rgba.empty() || m_w <= 0 || m_h1 <= 0)
        return;
    
    if (m_waterfallQuad.texture->pixelSize() != QSize(m_w, m_h1)) {
        return;
    }

    QImage rowImage(m_w, 1, QImage::Format_RGBA8888);
    std::memcpy(rowImage.bits(), rgba.data(), rgba.size() * sizeof(uint32_t));

    // Pass the underlying image memory to the description constructor manually
    QRhiTextureSubresourceUploadDescription sub(
        rowImage.constBits(),
        static_cast<qsizetype>(rowImage.sizeInBytes())
    );
    sub.setSourceSize(QSize(m_w, 1));
    sub.setDestinationTopLeft(QPoint(0, m_waterfallRowOffset));

    QRhiTextureUploadEntry const entry(0, 0, sub);
    u->uploadTexture(m_waterfallQuad.texture.get(),
                     QRhiTextureUploadDescription({entry}));

    // Advance and wrap our visual ring buffer tracker
    m_waterfallRowOffset = (m_waterfallRowOffset + 1) % std::max(1, m_h1);
}

// Converts the RDP-simplified m_points (widget pixel space, spectrum
// sub-area) into clip-space line-strip geometry and uploads it, along
// with the trace color, ready for render()'s draw call
void CPlotter::buildSpectrumGeometry(QRhiResourceUpdateBatch *u) {
    if (!m_rhi || !m_lineVBuf || m_points.size() < 2) {
        m_lineVertexCount = 0;
        return;
    }

    int const totalH = m_h1 + m_h2 + 30;

    std::vector<float> verts;
    verts.reserve(m_points.size() * 2);

    for (auto const &pt : m_points) {
        float const ndcX = (float(pt.x()) / std::max(1, m_w)) * 2.0f - 1.0f;

        // Shift data down by the true 30-pixel top scale bar offset
        float const yInWidget = float(30) + float(m_h1) + float(pt.y());
        float const ndcY = (yInWidget / std::max(1, totalH)) * 2.0f - 1.0f;
        auto const clip = toClipSpace(m_rhi, ndcX, ndcY);
        verts.push_back(float(clip.x()));
        verts.push_back(float(clip.y()));
    }

    qCDebug(plotter_js8) << "spectrum trace: m_h1=" << m_h1 << "m_h2=" << m_h2
                          << "first vert=" << QPointF(verts[0], verts[1])
                          << "last vert=" << QPointF(verts[verts.size()-2], verts[verts.size()-1]);

    qsizetype const bytes = verts.size() * sizeof(float);
    if (bytes > m_lineVBuf->size()) {
        // Grow the buffer rather than truncating the trace
        m_lineVBuf->destroy();
        m_lineVBuf->setSize(bytes);
        m_lineVBuf->create();
    }

    u->updateDynamicBuffer(m_lineVBuf.get(), 0, bytes, verts.data());

    float const color[4] = {float(m_lineColor.redF()), float(m_lineColor.greenF()),
                            float(m_lineColor.blueF()), float(m_lineColor.alphaF())};
    u->updateDynamicBuffer(m_lineUniformBuf.get(), 0, sizeof(color), color);

    m_lineVertexCount = static_cast<int>(verts.size() / 2);
}

void CPlotter::drawDecodeLine(QColor const &color, int const ia, int const ib) {

    auto const x1 = xFromFreq(ia);
    auto const x2 = xFromFreq(ib);
    int const row = (m_waterfallRowOffset - 1 + std::max(1, m_h1)) % std::max(1, m_h1);

    QImage marker(m_w, 1, QImage::Format_RGBA8888);
    marker.fill(Qt::transparent);
    QPainter p(&marker);
    p.setPen(color);
    p.drawLine(qMin(x1, x2), 0, qMax(x1, x2), 0);
    p.end();

    std::vector<uint32_t> pixels(m_w);
    std::memcpy(pixels.data(), marker.constBits(), m_w * sizeof(uint32_t));

    if (m_rhi) {
        QRhiResourceUpdateBatch* u = pendingUpdateBatch();
        QRhiTextureSubresourceUploadDescription sub(
            pixels.data(), static_cast<qsizetype>(pixels.size() * sizeof(uint32_t)));
        sub.setSourceSize(QSize(m_w, 1));
        sub.setDestinationTopLeft(QPoint(0, row));
        if (!m_rhi || !m_waterfallQuad.texture || m_w <= 0 ||
            m_waterfallQuad.texture->pixelSize() != QSize(m_w, m_h1))
            return;
        u->uploadTexture(m_waterfallQuad.texture.get(),
                         QRhiTextureUploadDescription({{0, 0, sub}}));
    }

    update();
}

void CPlotter::drawHorizontalLine(QColor const &color, int const x,
                                  int const width) {
    drawDecodeLine(color, x, width <= 0 ? m_w : x + width);
}

// CPU-side content builders
void CPlotter::drawMetrics() {
    if (m_scaleQuad.image.isNull())
        return;

    m_scaleQuad.image.fill(qRgba(255, 255, 255, 255));

    QPainter p(&m_scaleQuad.image);

    p.setPen(Qt::black);
    p.drawRect(0, 0, m_w, 30);

    auto const fSpan = m_w * m_freqPerPixel;
    auto const fpd = freqPerDiv(fSpan);
    float const ppdV = fpd / m_freqPerPixel;
    std::size_t const hdivs = fSpan / fpd + 1.9999f;
    int const fOffset = ((m_startFreq + fpd - 1) / fpd) * fpd;
    auto const xOffset = float(fOffset - m_startFreq) / fpd;
    std::size_t const nMajor = hdivs - 1;
    std::size_t const nMinor = fpd == 200 ? 4 : 5;
    float const ppdVM = ppdV / nMinor;
    float const ppdVL = ppdV / 2;

    for (std::size_t iMajor = 0; iMajor < nMajor; iMajor++) {
        auto const rMajor = (xOffset + iMajor) * ppdV;
        auto const xMajor = static_cast<int>(rMajor);
        p.drawLine(xMajor, 18, xMajor, 30);

        for (std::size_t iMinor = 1; iMinor < nMinor; iMinor++) {
            auto const xMinor = static_cast<int>(rMajor + iMinor * ppdVM);
            p.drawLine(xMinor, 22, xMinor, 30);
        }

        if (xMajor > 70) {
            p.drawText(QRect(xMajor - static_cast<int>(ppdVL), 0,
                             static_cast<int>(ppdV), 20),
                       Qt::AlignCenter,
                       QString::number(fOffset + iMajor * fpd));
        }
    }

    auto const bandX = [this](float const start, int const range) {
        return std::make_pair(xFromFreq(start), xFromFreq(start + range));
    };

    auto const drawBand = [this, &p](auto const &bandX) {
        auto const [x1, x2] = bandX;
        if (x1 <= m_w && x2 > 0) {
            p.drawLine(x1 + 1, 26, x2 - 2, 26);
            p.drawLine(x1 + 1, 28, x2 - 2, 28);
        }
    };

    p.setPen(QPen(BAND_EDGE, 3));
    drawBand(bandX(0.0f, 4000));
    p.setPen(QPen(BAND_WARN, 3));
    drawBand(bandX(500.0f, 2500));
    p.setPen(QPen(BAND_GOOD, 3));
    drawBand(bandX(1000.0f, 1500));

    if (in30MBand()) {
        auto const wspr = bandX(1.0e6f * (WSPR_START - m_dialFreq), WSPR_RANGE);
        auto font = QFont();
        font.setBold(true);
        font.setPointSize(10);

        p.setFont(font);
        p.setPen(QPen(BAND_WSPR, 3));
        drawBand(wspr);
        p.drawText(QRect(wspr.first, 0, wspr.second - wspr.first, 25),
                   Qt::AlignHCenter | Qt::AlignBottom, "WSPR");
    }
    p.end();

    m_scaleQuad.contentDirty = true;

    if (!m_overlayQuad.image.isNull()) {
        QLinearGradient gradient(0, 0, 0, m_h2);
        gradient.setColorAt(1, Qt::black);
        gradient.setColorAt(0, Qt::darkBlue);

        m_overlayQuad.image.fill(Qt::black);
        QPainter op(&m_overlayQuad.image);

        op.setBrush(gradient);
        op.drawRect(0, 0, m_w, m_h2);
        op.setBrush(Qt::SolidPattern);
        op.setPen(QPen(Qt::darkGray, 1, Qt::DotLine));

        auto const x0 = static_cast<int>(
            fractionalPart((float)m_startFreq / fpd) * ppdV + 0.5f);

        for (std::size_t i = 1; i < hdivs; i++) {
            if (auto const x = static_cast<int>(i * ppdV) - x0;
                x >= 0 && x <= m_w) {
                op.drawLine(x, 0, x, m_h2);
            }
        }

        float const ppdH = (float)m_h2 / VERT_DIVS;
        for (std::size_t i = 1; i < VERT_DIVS; i++) {
            auto const y = static_cast<int>(i * ppdH);
            op.drawLine(0, y, m_w, y);
        }

        op.end();
        m_overlayQuad.contentDirty = true;
    }
}

void CPlotter::updateFilterQuadGeometry() {
    auto const toNdcX = [this](int x) -> float {
        return 2.0f * (float(x) / std::max(1, m_w)) - 1.0f;
    };

    for (int i = 0; i < 2; ++i) {
        if (!m_filterQuad[i].image.isNull()) {
            int w = m_filterQuad[i].image.width();
            int x = (i == 0) ? 0 : xFromFreq(m_filterCenter + m_filterWidth / 2);

            float ndcLeft  = toNdcX(x);
            float ndcRight = toNdcX(x + w);

            float ndcTop    = m_overlayQuad.ndcRect.top();
            float ndcHeight = m_overlayQuad.ndcRect.height() + m_waterfallQuad.ndcRect.height();

            m_filterQuad[i].ndcRect = QRectF(ndcLeft, ndcTop, (ndcRight - ndcLeft), ndcHeight);
        }
    }
}

void CPlotter::drawFilter() {
    if (!(m_filterEnabled && m_filterWidth > 0 && !size().isEmpty()))
        return;

    auto const filterImage = [height = m_h1 + m_h2,   // was: size().height()
                              fill = QColor(0, 0, 0,
                                           std::clamp(m_filterOpacity, 0, 255))](
                                 int const width, int const lineX) {
        if (QSize const sz(width, height); sz.isEmpty()) {
            return QImage();
        } else {
            QImage img(sz, QImage::Format_RGBA8888);
            img.fill(fill);

            QPainter p(&img);
            p.setPen(Qt::yellow);
            p.drawLine(lineX, 1, lineX, height);
            p.end();

            return img;
        }
    };

    auto const width = m_filterWidth / 2.0f;
    auto const start = xFromFreq(m_filterCenter - width);
    auto const end = xFromFreq(m_filterCenter + width);

    m_filterQuad[0].image = filterImage(start, start);
    m_filterQuad[1].image = filterImage(size().width() - end, 0);

    for (auto &q : m_filterQuad) {
        q.structureDirty = true;
        q.contentDirty = true;
    }
    updateFilterQuadGeometry();
}

void CPlotter::drawDials() {
    if (auto const height = size().height() - 30; height > 0) {
        auto const width = static_cast<int>(
            JS8::Submode::bandwidth(m_nSubMode) / m_freqPerPixel + 0.5f);

        auto const dialImage = [size = QSize(width, height),
                                rect = QRect(1, 1, width - 2, height - 2)](
                                   QColor const &color, QBrush const &brush) {
            QImage img(size, QImage::Format_RGBA8888);
            img.fill(Qt::transparent);

            QPainter p(&img);
            p.setBrush(brush);
            p.setPen(QPen(QBrush(color), 2, Qt::SolidLine, Qt::SquareCap,
                          Qt::MiterJoin));
            p.drawRect(rect);
            p.end();

            return img;
        };

        m_dialQuad[0].image = dialImage(
            Qt::red, QBrush(QColor(255, 255, 255, 75), Qt::Dense4Pattern));
        m_dialQuad[1].image = dialImage(Qt::white, Qt::transparent);

        for (auto &q : m_dialQuad) {
            q.structureDirty = true;
            q.contentDirty = true;
        }
    }
}

// Rebuilds the waterfall ring buffer from the replot history
void CPlotter::replot() {
    if (m_w <= 0 || m_h1 <= 0 || !m_rhi)
        return;

    QRhiResourceUpdateBatch* u = pendingUpdateBatch();
    m_waterfallRowOffset = 0;

    auto y = 0;
    for (auto &&v : m_replot) {
        std::vector<uint32_t> row(m_w, 0xFF000000);

        std::visit([this, &row](auto const &v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, QString>) {
                QImage img(m_w, 1, QImage::Format_RGBA8888);
                img.fill(Qt::green);
                QPainter p(&img);
                p.setPen(Qt::white);
                p.drawText(5, 0, v);
                p.end();
                std::memcpy(row.data(), img.constBits(), m_w * sizeof(uint32_t));
            } else if constexpr (std::is_same_v<T, WF::SWide>) {
                auto const end = std::min(m_w, static_cast<int>(v.size()));
                for (auto x = 0; x < end; ++x) {
                    QColor col = m_colors[m_scaler1D(v[x])];
                    row[x] = (uint32_t(col.red())   << 0)  |
                             (uint32_t(col.green()) << 8)  |
                             (uint32_t(col.blue())  << 16) |
                             (uint32_t(255)         << 24);
                }
            }
        }, v);

        uploadWaterfallRow(row, u);
        y++;
        if (y >= m_h1)
            break;
    }
    update();
}

// Debounced resize handler: recomputes layout and rebuilds every CPU-side
// image; actual GPU (re)allocation in ensureGpuState()
void CPlotter::resize() {
    if (!size().isValid() || size().isEmpty() || size().width() <= 0 || size().height() <= 30)
        return;

    m_w = size().width();
    m_h2 = m_percent2D * (size().height() - 30) / 100.0;
    m_h1 = size().height() - m_h2;

    m_replot.resize(m_h1);
    m_scaler2D.rescale();

    m_waterfallQuad.image = QImage(m_w, m_h1, QImage::Format_RGBA8888);
    m_waterfallQuad.image.fill(Qt::black);
    m_overlayQuad.image = QImage(m_w, m_h2, QImage::Format_RGBA8888);
    m_scaleQuad.image = QImage(m_w, 30, QImage::Format_RGBA8888);

    if (m_rhi) {
        m_waterfallQuad.texture.reset();
        m_waterfallQuad.uniformBuf.reset();
        m_waterfallQuad.srb.reset();
        m_waterfallQuad.structureDirty = true;

        QRhiResourceUpdateBatch *u = pendingUpdateBatch();
        ensureQuadGpuState(m_waterfallQuad, QSize(m_w, m_h1), u);
    }

    float const totalH = float(size().height());
    m_scaleQuad.ndcRect = QRectF(-1.0f, 1.0f - (2.0f * 30.0f / totalH), 2.0f, 2.0f * 30.0f / totalH);
    m_waterfallQuad.ndcRect = QRectF(-1.0f, m_scaleQuad.ndcRect.top() - (2.0f * m_h1 / totalH), 2.0f, 2.0f * m_h1 / totalH);
    // Anchor the dark blue spectrum grid background to the absolute bottom edge (-1.0f)
    m_overlayQuad.ndcRect = QRectF(-1.0f, -1.0f, 2.0f, 2.0f * m_h2 / totalH);

    m_overlayQuad.structureDirty = true;
    m_scaleQuad.structureDirty = true;
    m_waterfallRowOffset = 0;

    drawDials();
    drawFilter();
    drawMetrics();

    replot();

    auto toNdcX = [this](int x) -> float {
        return 2.0f * (float(x) / std::max(1, m_w)) - 1.0f;
    };

    // Using absolute width scaling factors rather than subtracted clips ensures
    // that the selection rectangles track slider inputs
    for (int i = 0; i < 2; ++i) {
        if ((m_dialQuad[i].structureDirty || m_dialQuad[i].contentDirty) && !m_dialQuad[i].image.isNull()) {
            int x = (i == 0) ? xFromFreq(m_dialFreq) : std::max(0, m_lastMouseX);
            int w = m_dialQuad[i].image.width();
            int h = m_dialQuad[i].image.height();
            
            float ndcLeft  = toNdcX(x);
            float ndcWidth = 2.0f * (float(w) / std::max(1, m_w));
            float ndcTop    = m_waterfallQuad.ndcRect.top();
            float ndcHeight = m_waterfallQuad.ndcRect.height(); // Match the waterfall's height

            m_dialQuad[i].ndcRect = QRectF(ndcLeft, ndcTop, ndcWidth, ndcHeight);
        }
    }
    updateFilterQuadGeometry();
}

bool CPlotter::shouldDrawSpectrum(WF::State const state) const {
    if (m_overlayQuad.image.isNull())
        return false;

    return m_spectrum == Spectrum::Current ? state.testFlag(WF::Sink::Current)
                                           : state.testFlag(WF::Sink::Summary);
}

bool CPlotter::in30MBand() const {
    return (m_dialFreq >= BAND_30M_START && m_dialFreq <= BAND_30M_END);
}

int CPlotter::xFromFreq(float const f) const {
    return std::clamp(
        static_cast<int>((f - m_startFreq) / m_freqPerPixel + 0.5f), 0, m_w);
}

float CPlotter::freqFromX(int const x) const {
    return m_startFreq + x * m_freqPerPixel;
}

// Mouse / wheel handling
void CPlotter::leaveEvent(QEvent *event) {
    m_lastMouseX = -1;
    event->ignore();
}

void CPlotter::wheelEvent(QWheelEvent *event) {
    auto const y = event->angleDelta().y();

    if (auto const d = ((y > 0) - (y < 0))) {
        Q_EMIT changeFreq(event->modifiers() & Qt::ControlModifier
                              ? freq() + d
                              : freq() / 10 * 10 + d * 10);
    } else {
        event->ignore();
    }
}

void CPlotter::mouseMoveEvent(QMouseEvent *event) {
    m_lastMouseX = std::clamp(static_cast<int>(event->position().x()), 0, m_w);

    // Force the tracking dial layout to recompute its coordinate rectangle
    m_dialQuad[1].structureDirty = true;

    update();
    event->ignore();

    QToolTip::showText(
        event->globalPosition().toPoint(),
        QString::number(static_cast<int>(freqFromX(m_lastMouseX))), this);
}

void CPlotter::mouseReleaseEvent(QMouseEvent *event) {
    if (Qt::LeftButton == event->button()) {
        Q_EMIT changeFreq(static_cast<int>(freqFromX(m_lastMouseX)));
    } else {
        event->ignore();
    }
}

// Setters
void CPlotter::setBinsPerPixel(int const binsPerPixel) {
    if (m_binsPerPixel != binsPerPixel) {
        m_binsPerPixel = std::max(1, binsPerPixel);
        m_freqPerPixel = m_binsPerPixel * FFT_BIN_WIDTH;
        m_scaler1D.rescale();
        drawMetrics();
        drawFilter();
        drawDials();
        update();
    }
}

void CPlotter::setColors(Colors const &colors) {
    if (m_colors != colors) {
        m_colors = colors;
        replot();
    }
}

void CPlotter::setDialFreq(float const dialFreq) {
    if (m_dialFreq != dialFreq) {
        m_dialFreq = dialFreq;
        drawMetrics(); // Refreshes numbers and tick lines on CPU
        m_scaleQuad.contentDirty = true;
        m_dialQuad[0].structureDirty = true;
        m_dialQuad[0].contentDirty = true;
        
        update(); // Execute QRhiWidget render pass
    }
}

void CPlotter::setFilter(int const filterCenter, int const filterWidth) {
    if (m_filterCenter != filterCenter || m_filterWidth != filterWidth) {
        m_filterCenter = filterCenter;
        m_filterWidth = filterWidth;
        drawFilter();
        update();
    }
}

void CPlotter::setFilterEnabled(bool const filterEnabled) {
    if (m_filterEnabled != filterEnabled) {
        m_filterEnabled = filterEnabled;
        drawFilter();
        update();
    }
}

void CPlotter::setFilterOpacity(int const filterOpacity) {
    if (m_filterOpacity != filterOpacity) {
        m_filterOpacity = filterOpacity;
        drawFilter();
        update();
    }
}

void CPlotter::setFreq(int const freq) {
    if (m_freq != freq) {
        m_freq = freq;
        drawMetrics();
        
        // Ensure frequency shifts refresh the structural matrices
        m_scaleQuad.contentDirty = true;
        m_dialQuad[0].structureDirty = true;
        m_dialQuad[0].contentDirty = true;
        
        update();
    }
}

void CPlotter::setPercent2D(int percent2D) {
    if (m_percent2D != percent2D) {
        m_percent2D = percent2D;
        resize();
        update();
    }
}

void CPlotter::setPlotGain(int const plotGain) {
    if (m_scaler1D.gain() != plotGain) {
        m_scaler1D.setGain(plotGain);
        m_replotTimer->start();
    }
}

void CPlotter::setPlotZero(int const plotZero) {
    if (m_scaler1D.zero() != plotZero) {
        m_scaler1D.setZero(plotZero);
        m_replotTimer->start();
    }
}

void CPlotter::setStartFreq(int const startFreq) {
    if (m_startFreq != startFreq) {
        m_startFreq = startFreq;
        drawMetrics();
        drawFilter();
        
        m_scaleQuad.contentDirty = true;
        m_overlayQuad.contentDirty = true;
        m_filterQuad[0].contentDirty = true;
        m_filterQuad[1].contentDirty = true;
        
        update();
    }
}

void CPlotter::setSubMode(int const nSubMode) {
    if (m_nSubMode != nSubMode) {
        m_nSubMode = nSubMode;
        drawDials();
        update();
    }
}

void CPlotter::setWaterfallAvg(int const waterfallAvg) {
    if (m_waterfallAvg != waterfallAvg) {
        m_waterfallAvg = waterfallAvg;
        m_scaler1D.rescale();
    }
}
