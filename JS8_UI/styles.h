/**
 * @file styles.h
 * @brief Platform-specific label, button, widget, and dialog styles used by
 *        the UI_Constructor class to assemble JS8Call's user interface.
 *
 * @details
 * Provides QSS (Qt Style Sheet) stylesheet strings and a small helper widget
 * used to give JS8Call a native "look and feel" on macOS, Windows, and
 * Linux/other platforms.
 *  -# **General control styles**: buttonStyle() and logFrameStyle(), which
 *     style generic QPushButton/QToolButton widgets and the header bar's
 *     frequency display frame.
 *  -# **Styles namespace**: platform-specific constants for the two main
 *     regions of the UI:
 *       - Header Bar  -- LogWidgetStyle, DialFreqUpDownButtonStyle,
 *         OffsetSliderWidget, LabCallsignStyle, LabUTCStyle.
 *       - Control Bar -- MonitorTxButtonStyle, ControlButtonStyle,
 *         LogQSOButtonStyle, TuneButtonStyle, ModeButtonStyle.
 *
 * styles.h was added on 11 Apr, 2026 and is intended to be the
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

// =============================================================================
// General control styles
// =============================================================================

/**
 * @brief Returns a platform-native QPushButton/QToolButton stylesheet.
 *
 * Generates a stylesheet that approximates the native button conventions of
 * the target platform, using system-appropriate fonts, geometry, and accent
 * colors. Hover, pressed, and disabled pseudo-states are defined.
 * Also styles the drop-down menu indicator/padding used by
 * QToolButton#replyPushButton when it has an attached menu.
 *
 * @return A QSS QString suitable for use with @c QWidget::setStyleSheet(),
 *         or an empty QString() on unsupported platforms (falls back to the
 *         default Qt style).
 */
inline QString buttonStyle() {
#if defined(Q_OS_MACOS)
    return R"(
        QPushButton, QToolButton {
            background-color: #6699ff;
            color: black;
            border: none;
            border-radius: 6px;
            padding: 3px 9px;
            min-height: 12;
            max-height: 12px;
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
        QToolButton#replyPushButton::menu-button {
            width: 12px;
        }
        QToolButton#replyPushButton[popupMode="MenuButtonPopup"] {
            padding-right: 21px;
        }
        QToolButton#replyPushButton[popupMode="MenuButtonPopup"][layoutDirection="RightToLeft"] {
            padding-right: 9px;
            padding-left: 21px;
        }
    )";

#elif defined(Q_OS_WIN)
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
        QToolButton#replyPushButton::menu-button {
            width: 12px;
        }
        QToolButton#replyPushButton[popupMode="MenuButtonPopup"] {
            padding-right: 21px;
        }
        QToolButton#replyPushButton[popupMode="MenuButtonPopup"][layoutDirection="RightToLeft"] {
            padding-right: 9px;
            padding-left: 21px;
        }
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
       QToolButton#replyPushButton::menu-button {
           width: 12px;
       }
       QToolButton#replyPushButton[popupMode="MenuButtonPopup"] {
           padding-right: 21px;
       }
       QToolButton#replyPushButton[popupMode="MenuButtonPopup"][layoutDirection="RightToLeft"] {
           padding-right: 9px;
           padding-left: 21px;
       }
   )";

#else
    return QString();
#endif
}

/**
 * @brief Returns the stylesheet for the header bar's frequency display.
 *
 * Styles @c QFrame#frame (the header bar background) and
 * @c QLabel#currentFreq (the large frequency readout), including its font,
 * color, and border radius.
 *
 * @return A QSS QString suitable for use with @c QWidget::setStyleSheet().
 */

// =============================================================================
// Header bar frequency display
// =============================================================================

static inline QString logFrameStyle() {
#if defined(Q_OS_MACOS)
    return QStringLiteral("QFrame#frame { background-color: #F2F2F0; }"
                          "QLabel#currentFreq {"
                          " color: #39FF14;"
                          " background-color: black;"
                          " border-radius:6px; padding:0px 8px; "
                          " font-family: Monaco, 'Courier New', monospace;"
                          " font-size: 15pt;"
                          " font-weight: bold;"
                          " min-width: 150px;"
                          " max-width: 150px;"
                          " min-height: 20px;"
                          " max-height: 50px;"
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
    // Linux and other platforms
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

// =============================================================================
// Styles namespace
// =============================================================================

/**
 * @namespace Styles
 * @brief Platform-specific style constants for JS8Call's Header Bar and
 *        Control Bar.
 *
 * @details
 * This namespace provides constant QSS strings and one helper widget that
 * determine the appearance of the Header Bar and Control Bar. Exactly one
 * of the three preprocessor branches below (macOS, Windows, or
 * Linux/other) is compiled, always in that order, so each platform gets a
 * matching, complete set of definitions:
 *
 * **Header Bar**
 *  - @c LogWidgetStyle             -- background of the log/header frame.
 *  - @c DialFreqUpDownButtonStyle  -- small +/- frequency step buttons.
 *  - @c OffsetSliderWidget         -- labeled offset slider widget.
 *  - @c LabCallsignStyle           -- callsign label text.
 *  - @c LabUTCStyle                -- UTC clock label (LED-style readout).
 *
 * **Control Bar**
 *  - @c MonitorTxButtonStyle -- Monitor/Tx toggle button (green when
 *    monitoring, red while transmitting).
 *  - @c ControlButtonStyle   -- general on/off toggle buttons.
 *  - @c LogQSOButtonStyle    -- "Log QSO" button.
 *  - @c TuneButtonStyle      -- "Tune" button.
 *  - @c ModeButtonStyle      -- mode-select button.
 */
#if defined(Q_OS_MACOS)
namespace Styles {

// ---------------------------------------------------------------------------
// Header Bar
// ---------------------------------------------------------------------------

/// Background style for the header bar frame (macOS).
constexpr const char *LogWidgetStyle =
    "QFrame#logWidget { background-color: #F2F2F0; }";

/// Style for the small frequency up/down step buttons (macOS).
constexpr const char *DialFreqUpDownButtonStyle =
    "QPushButton {"
    "    background: transparent;"
    "    color: #000000;"
    "    font-size: 10pt;"
    "    padding: 0px;"
    "    border: none;"
    "    margin: 0px;"
    "    min-height: 14px;"
    "    max-height: 14px;"
    "    min-width: 10px;"
    "    max-width: 10px;"
    "}";

/**
 * @class OffsetSliderWidget
 * @brief A labeled "Offset:" slider used to select the RX/TX audio offset,
 *        in Hz, in the header bar.
 *
 * Composes a caption label, a horizontal QSlider (range 0-3000 Hz, default
 * 1500 Hz), and a value label that updates live as the slider moves. The
 * slider's own stylesheet is fixed here so its appearance does not change
 * with the system theme (light/dark mode).
 */
class OffsetSliderWidget : public QWidget {
  public:
    /**
     * @brief Constructs the widget and lays out caption, slider, and value
     *        label horizontally.
     * @param parent Optional parent widget.
     */
    explicit OffsetSliderWidget(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        auto *caption = new QLabel("Offset:", this);
        caption->setStyleSheet("QLabel { color: black; }");

        slider = new QSlider(Qt::Horizontal, this);
        slider->setStyleSheet(R"(
            QSlider {
                background: transparent;
                min-height: 14px;
                min-width: 100px;
                max-width: 100px;
            }
            QSlider::groove:horizontal {
                border: 1px solid #b0b0b0;
                height: 4px;
                background: #a5cdff;
                border-radius: 2px;
            }
            QSlider::handle:horizontal {
                background: #6699ff;
                border: 1px solid #c2c8d1;
                width: 12px;
                height: 12px;
                margin: -4px 0;
                border-radius: 6px;
            }
            QSlider::sub-page:horizontal {
                background: #a5cdff;
                border-radius: 2px;
            }
            QSlider::add-page:horizontal {
                background: #e0e0e0;
                border-radius: 2px;
            }
        )");
        slider->setRange(0, 3000);
        slider->setValue(1500);
        valueLabel = new QLabel("0 Hz", this);
        valueLabel->setStyleSheet("QLabel { color: black; }");
        valueLabel->setMinimumWidth(20);
        layout->addWidget(caption);
        layout->addWidget(slider);
        layout->addWidget(valueLabel);
        connect(slider, &QSlider::valueChanged, this, [this](int val) {
            valueLabel->setText(QString("%1 Hz").arg(val));
            if (onValueChanged) onValueChanged(val);
        });
    }

    /// @brief Returns the currently selected offset, in Hz.
    int offset() const { return slider->value(); }

    /// @brief Programmatically sets the offset, in Hz.
    /// @param hz New offset value; clamped to the slider's [0, 3000] range.
    void setValue(int hz) { slider->setValue(hz); }

    /// @brief Registers a callback invoked whenever the offset changes.
    /// @param cb Callback receiving the new offset in Hz.
    void setOnValueChanged(std::function<void(int)> cb) { onValueChanged = cb; }

  private:
    QSlider *slider;
    QLabel *valueLabel;
    std::function<void(int)> onValueChanged;
};

/// Style for the callsign label in the header bar (macOS).
constexpr const char *LabCallsignStyle = "QLabel {"
                                         "    font-size: 12pt;"
                                         "    line-height:12pt;"
                                         "    color : black;"
                                         "}";

/// Style for the UTC clock label; rendered as a black LED-style readout (macOS).
constexpr const char *LabUTCStyle =
    "QLabel {"
    "    border-radius:6px;"
    "    font-size: 12pt;"
    "    line-height:12pt;"
    "    font-family: Monaco, 'Courier New', monospace;"
    "    font-weight: bold;"
    "    background-color: black;"
    "    color : #39FF14;"
    "}";

// ---------------------------------------------------------------------------
// Control Bar
// ---------------------------------------------------------------------------

/// Style for the Tx toggle button: green when monitoring, red while
/// transmitting (macOS).
constexpr const char *MonitorTxButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "    color:black;"
    "}"
    "QPushButton[transmitting=\"true\"] {"
    "    background-color:#FF2222;"
    "    color:black;"
    "}";

/// General-purpose control bar toggle button style, with a disabled state
/// (macOS).
constexpr const char *ControlButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#22FF22;"
    "    color:black;"
    "}"
    "QPushButton:disabled {"
    "    background-color:lightgray;"
    "    color:gray;"
    "}"
    "QPushButton:checked:disabled {"
    "    background-color:#a0d8a0;"
    "    color:gray;"
    "}";

/// Style for the "Log QSO" button (macOS).
constexpr const char *LogQSOButtonStyle =
    "QPushButton {"
    "    background-color:#6699ff;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

/// Style for the "Tune" button: turns red while active (macOS).
constexpr const char *TuneButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#FF2222;"
    "    color:black;"
    "}";

/// Style for the mode-select button (macOS).
constexpr const char *ModeButtonStyle =
    "QPushButton {"
    "    padding:0.25em 0.25em; font-weight:bold;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:6px;"
    "    background-color:#6699ff;"
    "    color:black;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#6699ff;"
    "    color:black;"
    "}";

} // namespace Styles
#elif defined(Q_OS_WIN)
namespace Styles {

// ---------------------------------------------------------------------------
// Header Bar
// ---------------------------------------------------------------------------

/// Background style for the header bar frame (Windows).
constexpr const char *LogWidgetStyle =
    "QFrame#logWidget { background-color: #DDEEFF; }";

/// Style for the small frequency up/down step buttons (Windows).
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

/**
 * @class OffsetSliderWidget
 * @brief A labeled "Offset:" slider used to select the RX/TX audio offset,
 *        in Hz, in the header bar (Windows).
 */
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

    /// @brief Returns the currently selected offset, in Hz.
    int offset() const { return slider->value(); }

    /// @brief Programmatically sets the offset, in Hz.
    void setValue(int hz) { slider->setValue(hz); }

    /// @brief Registers a callback invoked whenever the offset changes.
    void setOnValueChanged(std::function<void(int)> cb) { onValueChanged = cb; }

  private:
    QSlider *slider;
    QLabel *valueLabel;
    std::function<void(int)> onValueChanged;
};

/// Style for the callsign label in the header bar (Windows).
constexpr const char *LabCallsignStyle = "QLabel {"
                                         "    font-size: 12pt;"
                                         "    line-height:12pt;"
                                         "    color : black;"
                                         "}";

/// Style for the UTC clock label; rendered as a black LED-style readout
/// (Windows).
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

// ---------------------------------------------------------------------------
// Control Bar
// ---------------------------------------------------------------------------

/// Style for the Tx toggle button: green when monitoring, red while
/// transmitting (Windows).
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

/// General-purpose control bar toggle button style, with a disabled state
/// (Windows).
constexpr const char *ControlButtonStyle =
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
    "QPushButton:disabled {"
    "    background-color:lightgray;"
    "    color:gray;"
    "}"
    "QPushButton:checked:disabled {"
    "    background-color:#a0d8a0;"
    "    color:gray;"
    "}";

/// Style for the "Log QSO" button (Windows).
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

/// Style for the "Tune" button: turns red while active (Windows).
constexpr const char *TuneButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:4px;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#FF2222;"
    "    color:black;"
    "}";

/// Style for the mode-select button (Windows).
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

} // namespace Styles
#else
namespace Styles {
// Linux and other platforms

// ---------------------------------------------------------------------------
// Header Bar
// ---------------------------------------------------------------------------

/// Background style for the header bar's log/status frame (Linux/other).
constexpr const char *LogWidgetStyle =
    "QFrame#logWidget { background-color: #F2F2F0; }";

/// Style for the small frequency up/down step buttons (Linux/other).
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

/**
 * @class OffsetSliderWidget
 * @brief A labeled "Offset:" slider used to select the RX/TX audio offset,
 *        in Hz, in the header bar (Linux/other).
 */
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

    /// @brief Returns the currently selected offset, in Hz.
    int offset() const { return slider->value(); }

    /// @brief Programmatically sets the offset, in Hz.
    void setValue(int hz) { slider->setValue(hz); }

    /// @brief Registers a callback invoked whenever the offset changes.
    void setOnValueChanged(std::function<void(int)> cb) { onValueChanged = cb; }

  private:
    QSlider *slider;
    QLabel *valueLabel;
    std::function<void(int)> onValueChanged;
};

/// Style for the callsign label in the header bar (Linux/other).
constexpr const char *LabCallsignStyle = "QLabel {"
                                         "    font-size: 14pt;"
                                         "    line-height:12pt;"
                                         "    color : black;"
                                         "}";

/// Style for the UTC clock label; rendered as a black LED-style readout
/// (Linux/other).
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

// ---------------------------------------------------------------------------
// Control Bar
// ---------------------------------------------------------------------------

/// Style for the Tx toggle button: green when monitoring, red while
/// transmitting (Linux/other).
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

/// General-purpose control bar toggle button style, with a disabled state
/// (Linux/other).
constexpr const char *ControlButtonStyle =
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
    "QPushButton:disabled {"
    "    background-color:lightgray;"
    "    color:gray;"
    "}"
    "QPushButton:checked:disabled {"
    "    background-color:#a0d8a0;"
    "    color:gray;"
    "}";

/// Style for the "Log QSO" button (Linux/other).
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

/// Style for the "Tune" button: turns red while active (Linux/other).
constexpr const char *TuneButtonStyle =
    "QPushButton {"
    "    background-color:lightgray;"
    "    color:black;"
    "    padding:0.25em 0.25em; font-weight:normal;"
    "    border-style:solid;"
    "    border-width:0px;"
    "    border-radius:0px;"
    "}"
    "QPushButton:hover {"
    "    background-color: #4d7fff;"
    "    color: white;"
    "}"
    "QPushButton:checked {"
    "    background-color:#FF2222;"
    "    color:black;"
    "}";

/// Style for the mode-select button (Linux/other).
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

} // namespace Styles
#endif
