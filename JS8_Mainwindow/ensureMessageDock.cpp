/** \file
 * @brief UI_Constructor::ensureMessageDock()
 * member function of the UI_Constructor class
 *
 * Lazily constructs the dockable Message Inbox panel (messageDock_ /
 * messagePanel_) on first use, including its custom toolbar (callsign
 * filter box, clear/float/close buttons) and the signal connections that
 * keep the "Show Message Inbox" menu action, dock visibility, and floating
 * window behavior in sync.
 *
 * @note Must be called before restoreState() in readSettings() so the
 * toolbar exists before saved dock visibility is restored.
 */

#include "JS8_UI/mainwindow.h"

void UI_Constructor::ensureMessageDock()
{
    if (messageDock_) return;

    messageDock_ = new QDockWidget(tr("Message Inbox"), this);
    messageDock_->setObjectName("messageInboxDock"); // important for save/restoreState

    // Choose where it can dock:
    messageDock_->setAllowedAreas(Qt::LeftDockWidgetArea |
                                  Qt::RightDockWidgetArea |
                                  Qt::BottomDockWidgetArea);

    // Choose behavior:
    messageDock_->setFeatures(QDockWidget::DockWidgetMovable |
                              QDockWidget::DockWidgetFloatable |
                              QDockWidget::DockWidgetClosable);

    // Initial placement:
    addDockWidget(Qt::RightDockWidgetArea, messageDock_);

    // Optional: closing hides (default); ensure no auto-delete:
    messageDock_->setAttribute(Qt::WA_DeleteOnClose, false);

    // --- Build the custom toolbar ---
    messagePanel_ = new MessagePanel(inboxPath(), this);

    // Container that holds the filter bar + the panel, becomes the dock's content
    auto *container = new QWidget(messageDock_);
    auto *vlay = new QVBoxLayout(container);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    auto *filterBar = new QWidget(container);
    auto *h = new QHBoxLayout(filterBar);
    h->setContentsMargins(6, 2, 6, 2);
    auto *lbl = new QLabel("Inbox:", filterBar);
    auto *edit = new QLineEdit(filterBar);
    edit->setPlaceholderText("Filter by callsign");
    {
        const int chars = 10;
        const QFontMetrics fm(edit->font());
        const int textWidth = fm.horizontalAdvance(QString(chars, QChar('W')));
        edit->setMaximumWidth(textWidth + 10);
        edit->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }
    auto *clearBtn = new QToolButton(filterBar);
    clearBtn->setText("<-clear");
    clearBtn->setToolTip("Clear filter");
    clearBtn->setAutoRaise(true);

    h->addStretch();
    h->addWidget(lbl);
    h->addWidget(edit);
    h->addWidget(clearBtn);
    filterBar->setLayout(h);

    vlay->addWidget(filterBar);
    vlay->addWidget(messagePanel_);
    container->setLayout(vlay);

    messageDock_->setWidget(container);

    m_inboxFilterEdit_ = edit;

    connect(edit, &QLineEdit::textChanged, this, [this](const QString &txt) {
        if (messagePanel_) messagePanel_->setCall(txt.trimmed().isEmpty() ? "%" : txt.trimmed().toUpper());
    });
    connect(clearBtn, &QToolButton::clicked, this, [this]() {
        if (m_inboxFilterEdit_) m_inboxFilterEdit_->clear();
    });

    // Make the menu action reflect visibility automatically:
    ui->actionShow_Message_Inbox->setCheckable(true);
    ui->actionShow_Message_Inbox->setChecked(messageDock_->isVisible());
    connect(messageDock_, &QDockWidget::visibilityChanged, this,
            [this](bool visible) {
                QSignalBlocker b(ui->actionShow_Message_Inbox);
                ui->actionShow_Message_Inbox->setChecked(visible);
            });
    
    /**
      * @brief Promote the floating Message Inbox dock to a real top-level window.
      *     Message Inbox enhancements by Chris-AC9KH, August 2026
      *
      * By default QDockWidget floats as a Qt::Tool-style panel. Reassigning
      * Qt::Window here gives it normal window chrome (minimize/maximize/close)
      * and lets it behave like an independent window rather than a utility
      * panel tied to MainWindow.
      *
      * @note Platform / windowing-system behavior differs significantly:
      *
      *   - MacOS: works fully. Native titlebar with working minimize, and the
      *     floating dock can be sent behind the main window by clicking on
      *     it, like any ordinary Cocoa window. Drag and double-click to
      *     redock both work from the titlebar.
      *
      *   - Windows: window chrome (min/max/close) renders correctly and
      *     drag/double-click redock work, but the floating dock can NOT be
      *     sent behind the main window by clicking it. This is a platform
      *     limitation, not a bug: the dock remains a Qt-parented child of
      *     MainWindow, and Windows always keeps an owned window above its
      *     owner in z-order regardless of the Qt window flags set here.
      *
      *   - Linux / X11 (confirmed on Linux Mint, XFCE): works partially.
      *     Clicking the main window correctly sends the floating dock
      *     behind it, and a taskbar icon is available to bring it back.
      *     Drag and double-click redock both work, however X11 treats the
      *     titlebar as a system window so double clicking maximizes the message
      *     panel. Drag-to-dock and double-click-to-dock works in the secondary
      *     Qt widget area under the titlebar.
      *
      *   - Linux / Wayland (confirmed on Ubuntu 26/GNOME/Wayland): the
      *     floating dock cannot be sent to the background — clicking the
      *     main window has no effect, and it always stays in the
      *     foreground, the same practical symptom as Windows though via a
      *     different mechanism (Wayland's own compositor stacking policy
      *     rather than Windows' owned-window z-order rule). In addition, the
      *     dock cannot be dragged while floating, and always appears
      *     re-centered on screen rather than at its prior position, because
      *     Wayland does not allow clients to set or query their own
      *     top-level window position. The compositor alone controls window
      *     placement, and interactive window moves require a client to
      *     issue an xdg_toplevel::move protocol request on pointer-down,
      *     which Qt's internal dock-widget drag logic does not do.
      *     Double-click-to-dock still works, since it's a discrete state
      *     change rather than a continuous position update. Detected at
      *     runtime via QGuiApplication::platformName() so this only affects
      *     genuine Wayland sessions, not Xwayland/X11 sessions on the same
      *     linux distribution.
      *
      * @warning Two fixes were evaluated and deliberately rejected:
      *   - Calling setParent(nullptr) on messageDock_ when floating, to
      *     escape the owned-window z-order rule on Windows. Removing the Qt
      *     parent breaks QMainWindowLayout's tracking of the dock, which
      *     broke docking, dragging, and closing entirely. Not viable.
      *   - Implementing the Wayland xdg_toplevel::move protocol request
      *     directly to restore floating-window drag under Wayland. This is
      *     real compositor-protocol-level work, not a Qt-level fix, and was
      *     judged not worth the added complexity for what the Wayland
      *     project itself may address in its own compositor implementations
      *     over time.
      */
    connect(messageDock_, &QDockWidget::topLevelChanged, this, [this](bool floating) {
        if (floating) {
            const bool isWayland = QGuiApplication::platformName().startsWith("wayland");
            if (!isWayland) {
                messageDock_->setWindowFlags(Qt::Window
                                              | Qt::WindowCloseButtonHint
                                              | Qt::WindowMinMaxButtonsHint);
                messageDock_->show();
            }
        }
    });

    // Handle reply function
    connect(messagePanel_, &MessagePanel::replyMessage, this,
                [this](const QString &text) {
                    addMessageText(text, true, true);
                    refreshInboxCounts();
                    displayCallActivity();
                });

    connect(messagePanel_, &MessagePanel::countsUpdated, this, [this]() {
            refreshInboxCounts();
            displayCallActivity();
        });

    // A manually-deleted stored message should stop generating notifications.
    connect(messagePanel_, &MessagePanel::messageDeleted, this,
            [this](int id) { removeStoredMessageNotification(id); });
}
