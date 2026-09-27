// -*- Mode: C++ -*-
///////////////////////////////////////////////////////////////////////////
// Some code in this file and accompanying files is based on work by
// Moe Wheatley, AE4Y, released under the "Simplified BSD License".
// For more details see the accompanying file LICENSE_WHEATLEY.TXT
///////////////////////////////////////////////////////////////////////////

#ifndef PLOTTER_H
#define PLOTTER_H

#include "JS8_Main/Flatten.h"
#include "JS8_UI/WF.h"
#include "RDP.h"

#include <QColor>
#include <QImage>
#include <QLoggingCategory>
#include <QPolygonF>
#include <QRhiWidget>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>

#include <rhi/qrhi.h>

#include <algorithm>
#include <array>
#include <boost/circular_buffer.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <variant>
#include <vector>

Q_DECLARE_LOGGING_CATEGORY(plotter_js8)

class CPlotter final : public QRhiWidget {
    Q_OBJECT

    // Scaler for the waterfall portion of the display; given a
    // y value, returns an index [0, 255) into the colors array.
    // Unchanged from the CPU version -- pure math, no painting.
    class Scaler1D {
        int const &m_avg;
        int const &m_bpp;
        int m_gain = 0;
        int m_zero = 0;
        float m_scale;

      public:
        Scaler1D(int const &avg, int const &bpp) : m_avg(avg), m_bpp(bpp) {
            rescale();
        }

        void rescale() {
            m_scale = 10.0f * std::sqrt(m_bpp * m_avg / 15.0f) *
                      std::pow(10.0f, 0.015f * m_gain);
        }

        int gain() const { return m_gain; }
        int zero() const { return m_zero; }

        void setGain(int const gain) {
            m_gain = gain;
            rescale();
        }
        void setZero(int const zero) { m_zero = zero; }

        inline auto operator()(float const value) const {
            return std::isnan(value)
                       ? 0
                       : std::clamp(m_zero + static_cast<int>(m_scale * value),
                                    0, 254);
        }
    };

    // Scaler for the spectrum portion of the display; given a
    // y value, returns a pixel offset into the spectrum view.
    // Unchanged from the CPU version.
    class Scaler2D {
        int const &m_h2;
        int m_gain = 0;
        int m_zero = 0;
        float m_scaledGain;
        float m_scaledZero;

      public:
        Scaler2D(int const &h2) : m_h2(h2) { rescale(); }

        void rescale() {
            m_scaledGain = m_h2 / 70.0f * std::pow(10.0f, 0.02f * m_gain);
            m_scaledZero = m_h2 * 0.9f - m_h2 / 70.0f * m_zero;
        }

        int gain() const { return m_gain; }
        int zero() const { return m_zero; }

        void setGain(int const gain) {
            m_gain = gain;
            rescale();
        }
        void setZero(int const zero) {
            m_zero = zero;
            rescale();
        }

        inline auto operator()(float const value) const {
            return m_scaledZero - m_scaledGain * value;
        }
    };

    // One GPU-resident, texture-backed quad: the waterfall, the spectrum
    // background/grid ("overlay"), the frequency scale, and each of the
    // dial/filter halves are all instances of this. Content is built into
    // `image` on the CPU (via QPainter, or via direct pixel writes for the
    // waterfall row path) and uploaded to `texture` in render().
    struct TexQuad {
        std::unique_ptr<QRhiTexture> texture;
        std::unique_ptr<QRhiBuffer> uniformBuf; // rect + opacity + rowOffset
        std::unique_ptr<QRhiShaderResourceBindings> srb;
        QImage image;      // CPU scratch surface; size == texture size
        QRectF ndcRect;    // placement in normalized device coords [-1,1]
        float opacity = 1.0f;
        bool contentDirty = false;  // image changed, needs re-upload
        bool structureDirty = true; // size changed, texture needs recreate
    };

  public:
    using Colors = QVector<QColor>;
    using Spectrum = WF::Spectrum;

    explicit CPlotter(QWidget *parent = nullptr);

    ~CPlotter() override;

    // Sizing
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

    // Inline accessors
    int binsPerPixel() const { return m_binsPerPixel; }
    int flatten() const { return m_flatten.live(); }
    int freq() const { return m_freq; }
    int percent2D() const { return m_percent2D; }
    int plot2dGain() const { return m_scaler2D.gain(); }
    int plot2dZero() const { return m_scaler2D.zero(); }
    int plotGain() const { return m_scaler1D.gain(); }
    int plotZero() const { return m_scaler1D.zero(); }
    Spectrum spectrum() const { return m_spectrum; }
    int startFreq() const { return m_startFreq; }

    int frequencyAt(int const x) const {
        return static_cast<int>(freqFromX(x));
    }

    // Inline manipulators
    void setFlatten(bool const flatten) { m_flatten(flatten); }
    void setPlot2dGain(int const plot2dGain) { m_scaler2D.setGain(plot2dGain); }
    void setPlot2dZero(int const plot2dZero) { m_scaler2D.setZero(plot2dZero); }
    void setSpectrum(Spectrum const spectrum) { m_spectrum = spectrum; }
    void drawLine(QString const &);
    void drawData(WF::SWide, WF::State);
    void drawDecodeLine(const QColor &, int, int);
    void drawHorizontalLine(const QColor &, int, int);
    void setBinsPerPixel(int);
    void setColors(Colors const &);
    void setDialFreq(float);
    void setFilter(int, int);
    void setFilterEnabled(bool);
    void setFilterOpacity(int);
    void setFreq(int);
    void setPercent2D(int);
    void setPlotGain(int);
    void setPlotZero(int);
    void setStartFreq(int);
    void setSubMode(int);
    void setWaterfallAvg(int);

  signals:

    void changeFreq(int);

  protected:
    // QRhiWidget rendering entry points -- replace paintEvent().
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;

    // Event handlers -- unchanged from the QWidget version.
    void resizeEvent(QResizeEvent *) override;
    void leaveEvent(QEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

  private:
    // Replot data storage; unchanged from the CPU version.
    using Replot = boost::circular_buffer<
        std::variant<std::monostate, QString, WF::SWide>>;

    // Accessors
    bool shouldDrawSpectrum(WF::State) const;
    bool in30MBand() const;
    int xFromFreq(float f) const;
    float freqFromX(int x) const;

    // CPU-side content builders -- these now paint into a TexQuad's QImage
    // and set contentDirty/structureDirty instead of drawing into a
    // QPixmap directly
    void drawMetrics();
    void drawFilter();
    void drawDials();
    void replot();
    void resize();

    // GPU helpers
    void ensureGpuState(QRhiResourceUpdateBatch *u);
    void ensureQuadGpuState(TexQuad &quad, QSize pixelSize,
                            QRhiResourceUpdateBatch *u);
    QRhiResourceUpdateBatch *pendingUpdateBatch();
    void uploadWaterfallRow(std::vector<uint32_t> const &rgba,
                            QRhiResourceUpdateBatch *u);
    void buildSpectrumGeometry(QRhiResourceUpdateBatch *u);
    void renderTexQuad(QRhiCommandBuffer *cb, TexQuad &quad);
    void updateFilterQuadGeometry();

    // Data members
    float m_dialFreq = 0.0f;
    int m_nSubMode = 0;
    int m_filterCenter = 0;
    int m_filterWidth = 0;
    int m_filterOpacity = 127;
    int m_percent2D = 0;
    int m_binsPerPixel = 2;
    int m_waterfallAvg = 1;
    int m_lastMouseX = -1;
    int m_line = std::numeric_limits<int>::max();
    int m_startFreq = 0;
    int m_freq = 0;
    int m_w = 0;
    int m_h1 = 0;
    int m_h2 = 0;
    bool m_filterEnabled = false;
    float m_freqPerPixel;

    RDP m_rdp;
    Scaler1D m_scaler1D;
    Scaler2D m_scaler2D;
    Colors m_colors;
    Replot m_replot;
    QPolygonF m_points;
    Flatten m_flatten;
    Spectrum m_spectrum = Spectrum::Current;
    QTimer *m_replotTimer;
    QTimer *m_resizeTimer;

    QString m_text;

    // GPU-side state
    QRhi *m_rhi = nullptr;
    QRhiResourceUpdateBatch *m_pendingUpdates = nullptr;
    int m_lineVertexCount = 0;
    int m_waterfallRowOffset = 0;
    std::vector<uint32_t> m_rowScratch;
    QColor m_lineColor = Qt::green;

    std::unique_ptr<QRhiBuffer>                 m_quadVBuf;
    std::unique_ptr<QRhiSampler>                m_sampler;
    std::unique_ptr<QRhiShaderResourceBindings> m_quadPipelineLayoutSrb;
    std::unique_ptr<QRhiShaderResourceBindings> m_lineSrb;
    std::unique_ptr<QRhiGraphicsPipeline>       m_quadPipeline;
    std::unique_ptr<QRhiGraphicsPipeline>       m_linePipeline;
    std::unique_ptr<QRhiBuffer>                 m_pipelineDummyLayoutBuf;
    std::unique_ptr<QRhiTexture>                m_pipelineDummyLayoutTex;
    
    QShader m_lineVertShader;
    QShader m_lineFragShader;
    QShader m_quadVertShader;
    QShader m_quadFragShader;

    std::unique_ptr<QRhiBuffer> m_lineVBuf;
    std::unique_ptr<QRhiBuffer> m_lineUniformBuf;

    TexQuad m_waterfallQuad;
    TexQuad m_overlayQuad;
    TexQuad m_scaleQuad;
    std::array<TexQuad, 2> m_dialQuad;
    std::array<TexQuad, 2> m_filterQuad;
};

#endif // PLOTTER_H
