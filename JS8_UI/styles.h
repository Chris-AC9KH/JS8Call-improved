/**
 * @file styles.h
 * @brief header file that defines platform specific label, button, widget and
 * dialog styles used in functions in the UI_Constructor class that assembles
 * the UI and provides the proper "look and feel" for each desktop platform
 *
 * styles.h was added on 11 Apr, 2026 and is intended to eventually become the
 * default StyleSheet configuration for the JS8Call user interface.
 */

#pragma once
#include <QtCore/QOperatingSystemVersion>
#include <QtCore/QString>
#include <QtGlobal>
#include <QtGui/QGuiApplication>
#include <QHBoxLayout>
#include <QSlider>
#include <QLabel>
#include <QWidget>

/**
 * @brief Provides platform-adaptive stylesheet strings for status bar QLabel
 * widgets.
 *
 * This module defines styling for QLabel-based status indicators,
 * with platform-specific geometry (border-radius, padding, border style)
 * targeting macOS, Windows, and Linux. A minimal fallback is provided for other
 * platforms, e.g. possibly compiling on BSD Unix, iOS, Android, etc..
 *
 * @section tx_status Transmit Status Appearance
 * The @c TxStatusAppearance enum represents four logical states for the TX
 * status label:
 * - @c Receiving    – Active receive; rendered with a green background, black text.
 * - @c Transmitting – Active transmit; rendered with a red background, black text.
 * - @c Decoding     – Decoding in progress; currently shares colors with @c
 * Receiving, but is defined separately to allow future change.
 * - @c IdleTimeout  – Idle or timed-out; rendered with a black background, white text.
 *
 * @fn static inline QString makeStyle(const QString& bg, const QString& fg)
 * @brief Constructs a platform-appropriate QSS stylesheet string for a QLabel.
 * @param bg Background color as a CSS color string.
 * @param fg Foreground (text) color as a CSS color string..
 * @return A QSS QString suitable for use with @c QWidget::setStyleSheet().
 *
 * @fn inline QString txStatusLabelStyle(TxStatusAppearance appearance)
 * @brief Returns the QSS stylesheet string for a given TX status state.
 * @param appearance The desired @c TxStatusAppearance state.
 * @return A QSS QString for the requested appearance, or an empty QString()
 *         for any unhandled @c default case (compiler safety fallback).
 */
static inline QString statusLabelStyle(const QString &bg = "#6699ff",
                                       const QString &fg = "#000000") {
#if defined(Q_OS_MACOS)
    return QStringLiteral("QLabel{background-color: %1; color: %2; "
                          "border-radius: 6px; padding: 2px 8px; "
                          "border: 1px solid rgba(0,0,0,0.15)}")
        .arg(bg, fg);
#elif defined(Q_OS_WIN)
    return QStringLiteral("QLabel{background-color:%1; color:%2; "
                          "border-radius:4px; padding:0px 8px; "
                          "border:1px solid rgba(0,0,0,0.25)}")
        .arg(bg, fg);
#else // Linux/other
    return QStringLiteral("QLabel{background-color:%1; color:%2; "
                          "border-radius:1px; padding:1px 6px; "
                          "border:1px inset rgba(0,0,0,0.18)}")
        .arg(bg, fg);
#endif
}

enum class TxStatusAppearance {
    Receiving,    // green on black text
    Transmitting, // red
    Decoding,     // same as Receiving
    IdleTimeout   // black bg, white fg
};

QString txStatusLabelStyle(TxStatusAppearance appearance);

static inline QString makeStyle(const QString &bg, const QString &fg) {
#if defined(Q_OS_LINUX)
    return QString("QLabel{background-color:%1; color:%2; "
                   "border-radius:1px; padding:1px 6px; "
                   "border:1px inset rgba(0,0,0,0.18);}")
        .arg(bg, fg);
#elif defined(Q_OS_WIN)
    return QString("QLabel{background-color:%1; color:%2; "
                   "border-radius:4px; padding:0px 8px; "
                   "border:1px solid rgba(0,0,0,0.25);}")
        .arg(bg, fg);
#elif defined(Q_OS_MACOS)
    return QString("QLabel{background-color:%1; color:%2; "
                   "border-radius:6px; padding:2px 8px; "
                   "border:1px solid rgba(0,0,0,0.15);}")
        .arg(bg, fg);
#else
    return QString("QLabel{background-color:%1; color:%2;}").arg(bg, fg);
#endif
}

inline QString txStatusLabelStyle(TxStatusAppearance appearance) {
    switch (appearance) {
    case TxStatusAppearance::Receiving:
        return makeStyle("#22ff22", "#000000");
    case TxStatusAppearance::Transmitting:
        return makeStyle("#ff2222", "#000000");
    case TxStatusAppearance::Decoding:
        return makeStyle("#22ff22", "#000000");
    case TxStatusAppearance::IdleTimeout:
        return makeStyle("#000000", "#ffffff");
    default:
        return QString(); // no style for compiler fallback
    }
}

/**
 * @brief Constructs a unified cross-platform QSS stylesheet for the
 * QProgressBar.
 *
 * Generates a stylesheet that produces a consistent progress bar appearance
 * across all platforms, with a white background (@c #ffffff) and a light-blue
 * chunk fill
 * (@c #a5cdff). Border is suppressed entirely; text is centered. The bar
 * dimensions are fully constrained via @c min-height / @c max-height to prevent
 * platform layout interference.
 *
 * @param small     If @c true, renders a compact variant (height: 10px, radius:
 * 3px); if @c false (default), renders the standard size (height: 14px, radius:
 * 5px).
 * @return A QSS QString suitable for use with @c QWidget::setStyleSheet().
 */
static inline QString progress_bar_stylesheet(bool small = false) {
    const QString base = QString("QProgressBar {"
                                 "  border: 0px;"
                                 "  background-color: #ffffff;"
                                 "  color: #000000;"
                                 "  text-align: center;"
                                 "  padding: 0px;"
                                 "  %1"
                                 "}"
                                 "QProgressBar::chunk {"
                                 "  background-color: #a5cdff;"
                                 "  border-radius: %2px;"
                                 "  %3"
                                 "}");

    const int height = small ? 10 : 14; // overall control height
    const int radius = small ? 3 : 5;   // roundness of the bar ends
    const QString barDim =
        QString("min-height:%1px; max-height:%1px; border-radius:%2px;")
            .arg(height)
            .arg(radius);
    const QString chunkDim = QString("min-height:%1px;").arg(height);
    return base.arg(barDim, QString::number(radius), chunkDim);
}

/**
 * @brief Returns a platform-native QPushButton/QToolButton stylesheet.
 *
 * Generates a stylesheet that approximates the native button conventions of the
 * specific platform, using system-appropriate fonts, geometry, and accent
 * colors. Hover, pressed, and disabled pseudo-states are defined for all active
 * variants. QToolButton's native menu-indicator glyph is suppressed since
 * platform styles (notably QMacStyle) draw it outside the stylesheet's box
 * model, producing an inconsistent look next to the styled button body;
 * callers relying on a popup menu should indicate this in the button's own
 * text (e.g. appending "▾") if a visual affordance is needed
 *
 * @return A QSS QString suitable for use with @c QWidget::setStyleSheet(),
 *         or an empty default Qt style on unsupported platforms.
 */
inline QString buttonStyle() {
#if defined(Q_OS_WIN)
    return R"(
        QPushButton, QToolButton {
            background-color: #6699ff;
            color: black;
            border: none;
            border-radius: 4px;
            padding: 3px 9px;
            min-height: 15px;
            max-height: 15px;
            font-family: "Segoe UI";
        }
        QPushButton:hover, QToolButton:hover {
            background-color: #4d7fff;
            color: white;
        }
        QPushButton:pressed, QToolButton:pressed {
            background-color: #003EAA;
        }
        QPushButton:disabled, QToolButton:disabled {
            background-color: #ececec;
            color: #888888;
        }
    )";

#elif defined(Q_OS_MACOS)
    return R"(
        QPushButton, QToolButton {
            background-color: #6699ff;
            color: black;
            border: none;
            border-radius: 6px;
            padding: 3px 9px;
            min-height: 15px;
            max-height: 15px;
            font-family: "-apple-system";
        }
        QPushButton:hover, QToolButton:hover {
            background-color: #4d7fff;
            color: white;
        }
        QPushButton:pressed, QToolButton:pressed {
            background-color: #003EAA;
        }
        QPushButton:disabled, QToolButton:disabled {
            background-color: #ececec;
            color: #888888;
        }
        QToolButton::menu-indicator { image: none; }
    )";

#elif defined(Q_OS_LINUX)
    return R"(
       QPushButton, QToolButton {
           background-color: #6699ff;
           color: black;
           border: none;
           border-radius: 5px;
           padding: 3px 9px;
           font-family: "Ubuntu", "Noto Sans";
       }
       QPushButton:hover, QToolButton:hover {
           background-color: #4d7fff;
       }
       QPushButton:pressed, QToolButton:pressed {
           background-color: #003EAA;
       }
       QPushButton:disabled, QToolButton:disabled {
           background-color: #ececec;
           color: #888888;
       }
   )";

#else
    return QString();
#endif
}

// defines the background, color, font and size of the header bar frequency
// display, the #else section contains the definitions for linux
static inline QString logFrameStyle() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("QFrame#frame { background-color: #F2F2F0; }"
                          "QLabel#currentFreq {"
                          " color: #39FF14;"
                          " background-color: black;"
                          " border-radius:6px; padding:0px 8px; "
                          " font-family: Monaco, 'Courier New', monospace;"
                          " font-size: 20pt;"
                          " font-weight: bold;"
                          " min-width: 200px;"
                          " min-height: 40px;"
                          "}");
#elif defined(Q_OS_WIN)
    return QStringLiteral("QFrame#frame { background-color: #DDEEFF; }"
                          "QLabel#currentFreq {"
                          " color: #39FF14;"
                          " background-color: black;"
                          " border-radius:4px; padding:0px 8px; "
                          " font-family: Consolas, 'Courier New', monospace;"
                          " font-size: 20pt;"
                          " font-weight: bold;"
                          " min-width: 200px;"
                          " min-height: 40px;"
                          "}");
#else
    return QStringLiteral("QFrame#frame { background-color: #F2F2F0; }"
                          "QLabel#currentFreq {"
                          " color: #39FF14;"
                          " background-color: black;"
                          " border-radius:0px; padding:0px 8px; "
                          " font-size: 28pt;"
                          " font-weight: bold;"
                          " min-width: 200px;"
                          " min-height: 40px;"
                          "}");
#endif
}

/**
 * @namespace Styles
 * @brief Defines platform-specific style constants for JS8Call's header bar controls.
 *
 * This namespace provides a set of constant strings that determine the appearance of
 * various header bar UI elements in JS8Call, including frequency widgets, buttons, and labels.
 * Each definition adapts to the conventions of MacOS, Windows, or other (which includes linux)
 * platforms, ensuring a consistent and native look and feel per-platform.
 *
 * @details
 * - Styles include visual treatments for log widgets, frequency buttons,
 *   status labels, and a variety of push buttons (grid, monitor, log, mode, spot, etc.).
 * - Platform differences (MacOS, Windows, other) are handled using preprocessor
 *   directives, with each block tailored to the target system's design conventions.
 * - For each supported platform, constants such as @c LogWidgetStyle,
 *   @c DialFreqUpDownButtonStyle, @c LabUTCStyle, and others are defined.
 */
#if defined(Q_OS_MACOS)
namespace Styles {

constexpr const char *LogWidgetStyle =
    "QFrame#logWidget { background-color: #F2F2F0; }";

constexpr const char *DialFreqUpDownButtonStyle =
    "QPushButton {"
    "    background-color: #000000;"
    "    color: #39FF14;"
    "    font-size: 10pt;"
    "    border:0px solid;"
    "    border-radius:2px;"
    "}"
    "QPushButton:pressed {"
    "    background-color: #222;"
    "}";

class OffsetSliderWidget : public QWidget {
  public:
    explicit OffsetSliderWidget(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QHBoxLayout(this);
        auto *caption = new QLabel("Offset:", this);
        caption->setStyleSheet("QLabel { color: black; }");
        slider = new QSlider(Qt::Horizontal, this);
        // Lock the QSlider's appearance regardless of the system theme
        slider->setStyleSheet(R"(
            QSlider {
                background: transparent;
                min-height: 24px;
            }
            QSlider::groove:horizontal {
                border: 1px solid #b0b0b0;
                height: 6px;
                background: #a5cdff;
                border-radius: 3px;
            }
            QSlider::handle:horizontal {
                background: #6699ff;
                border: 1px solid #c2c8d1;
                width: 16px;
                margin: -3px 0;
                border-radius: 6px;
            }
            QSlider::sub-page:horizontal {
                background: #a5cdff;
                border-radius: 3px;
            }
            QSlider::add-page:horizontal {
                background: #e0e0e0;
                border-radius: 3px;
            }
        )");
        slider->setRange(0, 3000);
        slider->setValue(1500);
        valueLabel = new QLabel("0 Hz", this);
        valueLabel->setStyleSheet("QLabel { color: black; }");
        valueLabel->setMinimumWidth(60);
        layout->addWidget(caption);
        layout->addWidget(slider, 1);
        layout->addWidget(valueLabel);
        connect(slider, &QSlider::valueChanged, this, [this](int val) {
            valueLabel->setText(QString("%1 Hz").arg(val));
            if (onValueChanged) onValueChanged(val);
        });
    }

    int offset() const { return slider->value(); }
    void setValue(int hz) { slider->setValue(hz); }
    void setOnValueChanged(std::function<void(int)> cb) { onValueChanged = cb; }

  private:
    QSlider *slider;
    QLabel *valueLabel;
    std::function<void(int)> onValueChanged;
};

constexpr const char *LabCallsignStyle = "QLabel {"
                                         "    font-size: 12pt;"
                                         "    line-height:12pt;"
                                         "    color : black;"
                                         "}";

constexpr const char *LabUTCStyle =
    "QLabel {"
    "    border-radius:6px;"
    "    font-size: 14pt;"
    "    line-height:14pt;"
    "    font-family: Monaco, 'Courier New', monospace;"
    "    font-weight: bold;"
    "    background-color: black;"
    "    color : #39FF14;"
    "}";

constexpr const char *ButtonGridStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "}";

constexpr const char *MonitorTxButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "    color:black;"
    "}"
    "QPushButton[transmitting=\"true\"] {"
    "    background-color:#FF2222;"
    "    color:black;"
    "}";

constexpr const char *MonitorButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "    color:black;"
    "}";

constexpr const char *LogQSOButtonStyle =
    "QPushButton {"
    "    background-color:#6699ff;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

constexpr const char *TuneButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

constexpr const char *ModeButtonStyle =
    "QPushButton {"
    "    padding:0.25em 0.25em; font-weight:bold;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "    background-color:#6699ff;"
    "    color:black;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

constexpr const char *SpotButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

}
#elif defined(Q_OS_WIN)
namespace Styles {

constexpr const char *LogWidgetStyle =
    "QFrame#logWidget { background-color: #DDEEFF; }";

constexpr const char *DialFreqUpDownButtonStyle =
    "QPushButton {"
    "    background-color: #000000;"
    "    color: #39FF14;"
    "    font-size: 12pt;"
    "    border:0px solid;"
    "    border-radius:2px;"
    "}"
    "QPushButton:pressed {"
    "    background-color: #222;"
    "}";

class OffsetSliderWidget : public QWidget {
  public:
    explicit OffsetSliderWidget(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QHBoxLayout(this);
        auto *caption = new QLabel("Offset:", this);
        caption->setStyleSheet("QLabel { color: black; }");
        slider = new QSlider(Qt::Horizontal, this);
        // Lock the QSlider's appearance regardless of the system theme
        slider->setStyleSheet(R"(
            QSlider {
                background: transparent;
                min-height: 24px;
            }
            QSlider::groove:horizontal {
                border: 1px solid #b0b0b0;
                height: 6px;
                background: #a5cdff;
                border-radius: 3px;
            }
            QSlider::handle:horizontal {
                background: #6699ff;
                border: 1px solid #c2c8d1;
                width: 16px;
                margin: -3px 0;
                border-radius: 5px;
            }
            QSlider::sub-page:horizontal {
                background: #a5cdff;
                border-radius: 3px;
            }
            QSlider::add-page:horizontal {
                background: #e0e0e0;
                border-radius: 3px;
            }
        )");
        slider->setRange(0, 3000);
        slider->setValue(1500);
        valueLabel = new QLabel("0 Hz", this);
        valueLabel->setStyleSheet("QLabel { color: black; }");
        valueLabel->setMinimumWidth(60);
        layout->addWidget(caption);
        layout->addWidget(slider, 1);
        layout->addWidget(valueLabel);
        connect(slider, &QSlider::valueChanged, this, [this](int val) {
            valueLabel->setText(QString("%1 Hz").arg(val));
            if (onValueChanged) onValueChanged(val);
        });
    }

    int offset() const { return slider->value(); }
    void setValue(int hz) { slider->setValue(hz); }
    void setOnValueChanged(std::function<void(int)> cb) { onValueChanged = cb; }

  private:
    QSlider *slider;
    QLabel *valueLabel;
    std::function<void(int)> onValueChanged;
};

constexpr const char *LabCallsignStyle = "QLabel {"
                                         "    font-size: 12pt;"
                                         "    line-height:12pt;"
                                         "    color : black;"
                                         "}";

constexpr const char *LabUTCStyle =
    "QLabel {"
    "    border-radius:4px;"
    "    font-size: 14pt;"
    "    line-height:14pt;"
    "    font-family: Consolas, 'Courier New', monospace;"
    "    font-weight: bold;"
    "    background-color: black;"
    "    color : #39FF14;"
    "}";

constexpr const char *ButtonGridStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "}";

constexpr const char *MonitorTxButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "    color:black;"
    "}"
    "QPushButton[transmitting=\"true\"] {"
    "    background-color:#FF2222;"
    "    color:black;"
    "}";

constexpr const char *MonitorButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "    color:black;"
    "}";

constexpr const char *LogQSOButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

constexpr const char *TuneButtonStyle = LogQSOButtonStyle; // Same as above

constexpr const char *ModeButtonStyle =
    "QPushButton {"
    "    padding:0.25em 0.25em; font-weight:bold;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "    background-color:#6699ff;"
    "    color:black;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

constexpr const char *SpotButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

}
#else
namespace Styles {

// background color of the header bar - must be the same as the frequency display above
constexpr const char *LogWidgetStyle =
    "QFrame#logWidget { background-color: #F2F2F0; }";

// definition of the frequency displqy for the up/down buttons
constexpr const char *DialFreqUpDownButtonStyle =
    "QPushButton {"
    "    background-color: #000000;"
    "    color: #39FF14;"
    "    font-size: 12pt;"
    "    border:1px solid;"
    "    border-radius:0px;"
    "}"
    "QPushButton:pressed {"
    "    background-color: #222;"
    "}";

class OffsetSliderWidget : public QWidget {
  public:
    explicit OffsetSliderWidget(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QHBoxLayout(this);
        auto *caption = new QLabel("Offset:", this);
        caption->setStyleSheet("QLabel { color: black; }");
        slider = new QSlider(Qt::Horizontal, this);
        // Lock the QSlider's appearance regardless of the system theme
        slider->setStyleSheet(R"(
            QSlider {
                background: transparent;
                min-height: 24px;
            }
            QSlider::groove:horizontal {
                border: 1px solid #b0b0b0;
                height: 6px;
                background: #a5cdff;
                border-radius: 3px;
            }
            QSlider::handle:horizontal {
                background: #6699ff;
                border: 1px solid #c2c8d1;
                width: 16px;
                margin: -3px 0;
                border-radius: 5px;
            }
            QSlider::sub-page:horizontal {
                background: #a5cdff;
                border-radius: 3px;
            }
            QSlider::add-page:horizontal {
                background: #e0e0e0;
                border-radius: 3px;
            }
        )");
        slider->setRange(0, 3000);
        slider->setValue(1500);
        valueLabel = new QLabel("0 Hz", this);
        valueLabel->setStyleSheet("QLabel { color: black; }");
        valueLabel->setMinimumWidth(60);
        layout->addWidget(caption);
        layout->addWidget(slider, 1);
        layout->addWidget(valueLabel);
        connect(slider, &QSlider::valueChanged, this, [this](int val) {
            valueLabel->setText(QString("%1 Hz").arg(val));
            if (onValueChanged) onValueChanged(val);
        });
    }

    int offset() const { return slider->value(); }
    void setValue(int hz) { slider->setValue(hz); }
    void setOnValueChanged(std::function<void(int)> cb) { onValueChanged = cb; }

  private:
    QSlider *slider;
    QLabel *valueLabel;
    std::function<void(int)> onValueChanged;
};

// definition of the display of the callsign label
constexpr const char *LabCallsignStyle = "QLabel {"
                                         "    font-size: 14pt;"
                                         "    line-height:12pt;"
                                         "    color : black;"
                                         "}";

// definition of the display of the of the date/time label
constexpr const char *LabUTCStyle =
    "QLabel {"
    "    border-radius:0px;"
    "    font-size: 18pt;"
    "    line-height:18pt;"
    "    font-family: \"DejaVu Sans Mono\", \"Liberation Mono\", \"Noto Mono\", \"Ubuntu Mono\", monospace;"
    "    font-weight: bold;"
    "    background-color: black;"
    "    color : #39FF14;"
    "}";

// defintion of the display of the button grid
constexpr const char *ButtonGridStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "}";

// defintion of the display of the Tx button
constexpr const char *MonitorTxButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "}"
    "QPushButton[transmitting=\"true\"] {"
    "    background-color:#FF2222;"
    "}";

// definition of the display of the Rx button
constexpr const char *MonitorButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "}";

// defintion of the display of the Log and Tune buttons
constexpr const char *LogQSOButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "}";

constexpr const char *TuneButtonStyle = LogQSOButtonStyle; // Same as above

// definition of the display of the Mode button
constexpr const char *ModeButtonStyle =
    "QPushButton {"
    "    padding:0.25em 0.25em; font-weight:bold;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "    background-color:#6699ff;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "}";

// defintion of the display of the Spot button
constexpr const char *SpotButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "}";

}
#endif

