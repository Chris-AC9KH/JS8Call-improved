// -*- Mode: C++ -*-
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "JS8_Audio/AudioDevice.h"
#include "JS8_Audio/NotificationAudio.h"
#include "JS8_Audio/SoundInput.h"
#include "JS8_Audio/SoundOutput.h"
#include "JS8_Include/EventFilter.h"
#include "JS8_Include/commons.h"
#include "JS8_Include/qpriorityqueue.h"
#include "JS8_JSC/JSC_checker.h"
#include "JS8_Logbook/LogBook.h"
#include "JS8_Main/APRSISClient.h"
#include "JS8_Main/ActivityDB.h"
#include "JS8_Main/AprsInboundRelay.h"
#include "JS8_Main/Bands.h"
#include "JS8_Main/DirectedMessageHighlighter.h"
#include "JS8_Main/DriftingDateTime.h"
#include "JS8_Main/FrequencyList.h"
#include "JS8_Main/Geodesic.h"
#include "JS8_Main/HBBlockingDB.h"
#include "JS8_Main/HelpTextWindow.h"
#include "JS8_Main/Inbox.h"
#include "JS8_Main/JS8MessageBox.h"
#include "JS8_Main/MessageClient.h"
#include "JS8_Main/MessageServer.h"
#include "JS8_Main/Modes.h"
#include "JS8_Main/MultiSettings.h"
#include "JS8_Main/PillRenderer.h"
#include "JS8_Main/ProcessThread.h"
#include "JS8_Main/Radio.h"
#include "JS8_Main/SelfDestructMessageBox.h"
#include "JS8_Main/SignalMeter.h"
#include "JS8_Main/StationList.h"
#include "JS8_Main/TxLoop.h"
#include "JS8_Main/Varicode.h"
#include "JS8_Main/qt_helpers.h"
#include "JS8_Main/revision_utils.h"
#include "JS8_Mode/DecodedText.h"
#include "JS8_Mode/Decoder.h"
#include "JS8_Mode/Detector.h"
#include "JS8_Mode/JS8.h"
#include "JS8_Mode/JS8Submode.h"
#include "JS8_Mode/Modulator.h"
#include "JS8_Network/NetworkAccessManager.h"
#include "JS8_Network/PSKReporter.h"
#include "JS8_Network/SpotClient.h"
#include "JS8_Network/TCPClient.h"
#include "JS8_Transceiver/Transceiver.h"
#include "JS8_Transceiver/TransceiverFactory.h"
#include "JS8_UDP/WSJTXMessageClient.h"
#include "JS8_UDP/WSJTXMessageMapper.h"
#include "JS8_UI/About.h"
#include "JS8_UI/Configuration.h"
#include "JS8_UI/WideGraph.h"
#include "JS8_UI/MessagePanel.h"
#include "LogQSO.h"
#include "MessageReplyDialog.h"
#include "styles.h"
#include "ui_mainwindow.h"

#include <QAbstractSocket>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QAudioDevice>
#include <QByteArrayView>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMdiSubWindow>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScopedPointer>
#include <QScrollBar>
#include <QSet>
#include <QSoundEffect>
#include <QStandardPaths>
#include <QStringBuilder>
#include <QStyleFactory>
#include <QtCore/QtGlobal>
#include <QTableWidget>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QThread>
#include <QTime>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUdpSocket>
#include <QUrl>
#include <QUuid>
#include <QVariant>
#include <QVector>
#include <QVersionNumber>
#include <QtConcurrent/QtConcurrentRun>
#include <QtGui>
#include <boost/crc.hpp>
#include <fftw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

#if QT_VERSION < QT_VERSION_CHECK(6,11, 1)
#error "Qt >= 6.11.1 is required to build JS8Call. Please upgrade your Qt toolchain."
#endif

Q_DECLARE_LOGGING_CATEGORY(mainwindow_js8)

extern int volatile itone[JS8_NUM_SYMBOLS]; // Audio tones for all Tx symbols

//--------------------------------------------------------------- mainwindow
// How often to poll the UI, in MS.
// Some things may depend on this being a divisor of 1000.
constexpr quint32 UI_POLL_INTERVAL_MS = 100;

namespace {
namespace Default {
constexpr Radio::Frequency DIAL_FREQUENCY = 14078000;
constexpr auto FREQUENCY = 1500;
constexpr auto SUBMODE = Varicode::JS8CallNormal;
} // namespace Default

namespace State {
constexpr auto RX = 1;
constexpr auto TX = 2;
} // namespace State
} // namespace

namespace Ui {
class UI_Constructor;
}

class QSettings;
class QLineEdit;
class QFont;
class QHostInfo;
class WideGraph;
class LogQSO;
class Transceiver;
class MessageClient;
class QTime;
class HelpTextWindow;
class SoundOutput;
class Modulator;
class SoundInput;
class Detector;
class AprsInboundRelay;
class MultiSettings;
class DecodedText;
class JSCChecker;
class MessagePanel;

using namespace std;
typedef std::function<void()> Callback;

class WSJTXMessageMapper; // Forward declaration

class UI_Constructor : public QMainWindow {
    Q_OBJECT;
    friend class WSJTXMessageMapper; // Allow WSJTXMessageMapper to access
                                     // private members

    struct CallDetail;
    struct CommandDetail;

  public:
    using Frequency = Radio::Frequency;
    using FrequencyDelta = Radio::FrequencyDelta;
    using Mode = Modes::Mode;

    explicit UI_Constructor(QString const &program_info,
                            QDir const &temp_directory, bool multiple,
                            MultiSettings *settings, QWidget *parent = nullptr);
    ~UI_Constructor();

  private:
    struct SortByReverse {
        QString by;
        bool reverse;
    };

    /**
     * Sometimes, buttons are triggered by code and this refers
     * to a momentary situation only.  Sometimes, the trigger
     * pertains to the long term processing, that is, it should
     * influence (shut off, mostly) the automatic transmit loops
     * for HB and CQ.
     *
     * In an ugly klugde, we try to distinguish these cases
     * via these three *IsLongterm variables.
     */
    bool m_stopTxButtonIsLongterm;
    bool m_hbButtonIsLongterm;
    bool m_cqButtonIsLongterm;

  public slots:
    void showSoundInError(const QString &errorMsg);
    void showSoundOutError(const QString &errorMsg);
    void showStatusMessage(const QString &statusMsg);
    void dataSink(qint64 frames); // JS8_Mainwindow/dataSink.cpp
    /**
     * The name `guiUpdate` suggests updating of the views from the models
     * (in MVC terms, but we don't do MVC in this project), animations and
     * stuff. While it indeed does that, this also contains controller code.
     */
    void guiUpdate();
    void setXIT(int n);
    void qsy(int hzDelta);
    void onDriftChanged(qint64 new_drift_ms);
    bool tryRestoreFreqOffset();
    void changeFreq(int);

    bool hasExistingMessageBufferToMe(int *pOffset);
    bool hasExistingMessageBuffer(int submode, int offset, bool drift,
                                  int *pPrevOffset);
    bool hasClosedExistingMessageBuffer(int offset);
    void logCallActivity(CallDetail d, bool spot = true);
    void logHeardGraph(QString from, QString to);
    QString lookupCallInCompoundCache(QString const &call);
    void cacheActivity(QString key);
    void switchActivityBucket(QString const &band);
    void restoreActivity(QString key);
    void clearActivity();
    void clearBandActivity();
    void clearRXActivity();
    void clearCallActivity();
    void createGroupCallsignTableRows(QTableWidget *table,
                                      const QString &selectedCall,
                                      bool &showIconColumn);
    void displayTextForFreq(QString text, int freq, QDateTime date, bool isTx,
                            bool isNewLine, bool isLast);
    void writeNoticeTextToUI(QDateTime date, QString text);
    int writeMessageTextToUI(QDateTime date, QString text, int freq, bool isTx,
                             int block = -1);
    bool isMessageQueuedForTransmit();
    bool isInDecodeDelayThreshold(int seconds);
    void prependMessageText(QString text);
    void addMessageText(QString text, bool clear = false,
                        bool selectFirstPlaceholder = false);
    void confirmThenEnqueueMessage(int timeout, int priority, QString message,
                                   int offset, Callback c); // JS8_Mainwindow/confirmThenEnqueueMessage.cpp
    void enqueueMessage(int priority, QString message, int offset, Callback c);
    void resetMessage();
    void resetMessageUI();
    void restoreMessage();
    void initializeDummyData(); // JS8_Mainwindow/initializeDummyData.cpp
    void initializeGroupMessage(); // JS8_Mainwindow/initializeGroupMessage.cpp
    bool ensureCallsignSet(bool alert = true);
    bool ensureKeyNotStuck(QString const &text);
    bool ensureNotIdle();
    bool ensureCanTransmit();
    bool ensureCreateMessageReady(const QString &text);
    QString createMessage(QString const &text, bool *pDisableTypeahead);
    QString appendMessage(QString const &text, bool isData,
                          bool *pDisableTypeahead);
    QString createMessageTransmitQueue(QString const &text, bool reset,
                                       bool isData, bool *pDisableTypeahead);
    void resetMessageTransmitQueue();
    QPair<QString, int> popMessageFrame();
    void tryNotify(const QString &key);
    void processDecodeEvent(JS8::Event::Variant const &); // JS8_Mainwindow/processDecodeEvent.cpp

    void updateCQButtonDisplay();
    void updateHBButtonDisplay();

  protected:
    void keyPressEvent(QKeyEvent *) override;
    void closeEvent(QCloseEvent *) override;
    void childEvent(QChildEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;

  private slots:
    void initialize_fonts();
    void on_menuModeJS8_aboutToShow();
    void on_menuControl_aboutToShow();
    void on_actionCheck_for_Updates_triggered();
    void on_actionEnable_Monitor_RX_toggled(bool checked);
    void on_actionEnable_Transmitter_TX_toggled(bool checked);
    void on_actionEnable_Reporting_SPOT_toggled(bool checked);
    void on_actionEnable_Tuning_Tone_TUNE_toggled(bool checked);
    void on_menuWindow_aboutToShow();
    void on_actionFocus_Message_Receive_Area_triggered();
    void on_actionFocus_Message_Reply_Area_triggered();
    void on_actionFocus_Band_Activity_Table_triggered();
    void on_actionFocus_Call_Activity_Table_triggered();
    void on_actionClear_All_Activity_triggered();
    void on_actionClear_Band_Activity_triggered();
    void on_actionClear_RX_Activity_triggered();
    void on_actionClear_Call_Activity_triggered();
    void on_actionSetOffset_triggered();
    void on_actionShow_Fullscreen_triggered(bool checked);
    void on_actionShow_Statusbar_triggered(bool checked);
    void on_actionShow_Frequency_Clock_triggered(bool checked);
    void on_actionShow_Band_Activity_triggered(bool checked);
    void on_actionShow_Band_Heartbeats_and_ACKs_triggered(bool checked);
    void on_actionShow_Call_Activity_triggered(bool checked);
    void on_actionShow_Waterfall_triggered(bool checked);
    void on_actionShow_Waterfall_Controls_triggered(bool checked);
    void on_actionShow_Waterfall_Time_Drift_Controls_triggered(bool checked);
    void on_actionReset_Window_Sizes_triggered();
    void on_actionSettings_triggered();
    void openSettings(int tab = 0);
    void prepareApi();
    void prepareSpotting();
    void on_spotButton_clicked(bool checked);
    void on_monitorButton_clicked(bool);
    void on_actionAbout_triggered();
    void resetPushButtonToggleText(QPushButton *btn);
    void on_stopTxButton_clicked();
    void on_dialFreqUpButton_clicked();
    void on_dialFreqDownButton_clicked();
    void on_actionAdd_Log_Entry_triggered();
    void on_actionOpen_log_directory_triggered();
    void on_actionCopyright_Notice_triggered();
    void on_actionUser_Guide_triggered();
    bool decode(qint32 k);
    bool isDecodeReady(int submode, qint32 k, qint32 k0,
                       qint32 *pCurrentDecodeStart, qint32 *pNextDecodeStart,
                       qint32 *pStart, qint32 *pSz, qint32 *pCycle);
    bool decodeEnqueueReady(qint32 k, qint32 k0);
    bool decodeEnqueueReadyExperiment(qint32 k, qint32 k0);
    bool decodeProcessQueue(qint32 *pSubmode);
    void decodeStart();
    void decodeBusy(bool b);
    void decodeDone();
    void on_startTxButton_toggled(bool checked);
    void toggleTx(bool start);
    void on_logQSOButton_clicked();
    void on_actionModeJS8HB_toggled(bool checked);
    void on_actionModeJS8Normal_triggered();
    void on_actionModeJS8Fast_triggered();
    void on_actionModeJS8Turbo_triggered();
    void on_actionModeJS8Slow_triggered();
    void on_actionModeJS8Ultra_triggered();
    void on_actionHeartbeatAcknowledgements_toggled(bool checked);
    void on_actionModeMultiDecoder_toggled(bool checked);
    void on_actionModeAutoreply_toggled(bool checked);
    bool canCurrentModeSendHeartbeat() const;
    void prepareMonitorControls();
    void prepareHeartbeatMode(bool enabled);
    void f11f12(int n);
    void on_actionErase_ALL_TXT_triggered();
    void on_actionErase_js8call_log_adi_triggered();
    void startTx();
    void stopTx();
    void stopTx2();
    void buildFrequencyMenu(QMenu *menu);
    void buildHeartbeatMenu(QMenu *menu);
    void buildCQMenu(QMenu *menu);
    void buildRepeatMenu(QMenu *menu, QPushButton *button, bool isLowInterval,
                         int *interval);
    void sendHB();
    void sendHeartbeatAck(QString to, int snr, QString extra);
    void on_hbMacroButton_toggled(bool checked);
    void on_hbMacroButton_clicked();
    void sendCQ(bool repeat = false);
    void on_cqMacroButton_toggled(bool checked);
    void on_cqMacroButton_clicked();
    void on_replyMacroButton_clicked();
    void on_snrMacroButton_clicked();
    void on_infoMacroButton_clicked();
    void on_statusMacroButton_clicked();
    void setShowColumn(QString tableKey, QString columnKey, bool value);
    bool showColumn(QString tableKey, QString columnKey, bool default_ = true);
    void buildShowColumnsMenu(QMenu *menu, QString tableKey);
    void setSortBy(QString key, QString value);
    QString getSortBy(QString const &key, QString const &defaultValue) const;
    SortByReverse getSortByReverse(QString const &key,
                                   QString const &defaultValue) const;
    void buildSortByMenu(QMenu *menu, QString key, QString defaultValue,
                         QList<QPair<QString, QString>> values);
    void buildBandActivitySortByMenu(QMenu *menu);
    void buildCallActivitySortByMenu(QMenu *menu);
    void buildQueryMenu(QMenu *, QString callsign); // JS8_Mainwindow/buildQueryMenu.cpp
    QMap<QString, QString> buildMacroValues();
    void buildColumnLabelMap();
    void buildSuggestionsMenu(QMenu *menu, QTextEdit *edit,
                              const QPoint &point);
    void buildSavedMessagesMenu(QMenu *menu);
    void buildRelayMenu(QMenu *menu);
    QAction *buildRelayAction(QString call);
    void buildEditMenu(QMenu *, QTextEdit *);
    void on_queryButton_pressed();
    void on_macrosMacroButton_pressed();
    void on_deselectButton_pressed();
    void on_tableWidgetRXAll_cellClicked(int row, int col);
    void on_tableWidgetRXAll_cellDoubleClicked(int row, int col);
    QString generateCallDetail(QString selectedCall);
    void on_tableWidgetCalls_cellClicked(int row, int col);
    void on_tableWidgetCalls_cellDoubleClicked(int row, int col);
    QList<QPair<QString, int>> buildMessageFrames(QString const &text,
                                                  bool isData,
                                                  bool *pDisableTypeahead);
    bool prepareNextMessageFrame();
    bool isFreqOffsetFree(int f, int bw);
    int findFreeFreqOffset(int fmin, int fmax, int bw);
    void setDrift(int n);
    void on_tuneButton_clicked(bool);
    void acceptQSO(QDateTime const &, QString const &call, QString const &grid,
                   Frequency dial_freq, QString const &mode,
                   QString const &submode, QString const &rpt_sent,
                   QString const &rpt_received, QString const &comments,
                   QString const &name, QDateTime const &QSO_date_on,
                   QString const &operator_call, QString const &my_call,
                   QString const &my_grid, QByteArray const &ADIF,
                   QVariantMap const &additionalFields);
    void on_readFreq_clicked();
    void on_outAttenuation_valueChanged(int);
    void rigOpen();
    void handle_transceiver_update(Transceiver::TransceiverState const &);
    void handle_transceiver_failure(QString const &reason);
    void band_changed();
    void monitor(bool);
    void end_tuning();
    void stop_tuning();
    void stopTuneATU();
    void auto_tx_mode(bool);
    void on_monitorButton_toggled(bool checked);
    void on_monitorTxButton_toggled(bool checked);
    void on_tuneButton_toggled(bool checked);
    void on_spotButton_toggled(bool checked);

    void emitPTT(bool on);
    void emitTones();
    void udpNetworkMessage(Message const &message);
    void tcpNetworkMessage(Message const &message);
    void networkMessage(Message const &message); // JS8_Mainwindow/networkMessage.cpp
    bool canSendNetworkMessage();
    void sendNetworkMessage(QString const &type, QString const &message);
    void sendNetworkMessage(QString const &type, QString const &message,
                            const QVariantMap &params);
    void pskReporterError(QString const &);
    void TxAgain();
    void checkVersion(bool alertOnUpToDate); // JS8_Mainwindow/checkVersion.cpp
    void checkStartupWarnings();
    void clearCallsignSelected();
    void refreshTextDisplay();

    void manualBandHop(const StationList::Station station);

  private:
    Q_SIGNAL void apiSetMaxConnections(int n);
    Q_SIGNAL void apiSetServer(QString host, quint16 port);
    Q_SIGNAL void apiStartServer();
    Q_SIGNAL void apiStopServer();

    Q_SIGNAL void aprsClientEnqueueSpot(QString by_call, QString from_call,
                                        QString grid, QString comment);
    Q_SIGNAL void aprsClientEnqueueThirdParty(QString by_call,
                                              QString from_call, QString text);
    /**
     * @brief Send a standard APRS message ACK frame.
     * @param from_call Source callsign for the ACK.
     * @param to_call Destination callsign being acknowledged.
     * @param messageId APRS message identifier to acknowledge.
     */
    Q_SIGNAL void aprsClientEnqueueAck(QString from_call, QString to_call,
                                       QString messageId);
    Q_SIGNAL void aprsClientSetSkipPercent(float skipPercent);
    Q_SIGNAL void aprsClientSetIncomingRelayEnabled(bool enabled);
    Q_SIGNAL void aprsClientSetServer(QString host, quint16 port);
    Q_SIGNAL void aprsClientSetPaused(bool paused);
    Q_SIGNAL void aprsClientSetLocalStation(QString mycall, QString passcode);
    Q_SIGNAL void aprsClientSendReports();

    Q_SIGNAL void pskReporterSendReport(bool);
    Q_SIGNAL void pskReporterSetLocalStation(QString, QString, QString);
    Q_SIGNAL void pskReporterAddRemoteStation(QString, QString,
                                              Radio::Frequency, QString, int,
                                              QDateTime);

    Q_SIGNAL void spotClientSetLocalStation(QString, QString, QString);
    Q_SIGNAL void spotClientEnqueueCmd(QString, QString, QString, QString,
                                       QString, QString, QString, int, Frequency, int,
                                       int);
    Q_SIGNAL void spotClientEnqueueSpot(QString, QString, int, Frequency, int, int);

    Q_SIGNAL void decodedLineReady(QByteArray t);
    Q_SIGNAL void playNotification(const QString &name);
    Q_SIGNAL void initializeNotificationAudioOutputStream(const QAudioDevice &,
                                                          unsigned) const;
    Q_SIGNAL void initializeAudioOutputStream(QAudioDevice, unsigned channels,
                                              unsigned msBuffered) const;
    Q_SIGNAL void stopAudioOutputStream() const;
    Q_SIGNAL void startAudioInputStream(QAudioDevice const &,
                                        int framesPerBuffer, AudioDevice *sink,
                                        AudioDevice::Channel) const;
    Q_SIGNAL void suspendAudioInputStream() const;
    Q_SIGNAL void resumeAudioInputStream() const;
    Q_SIGNAL void startDetector(AudioDevice::Channel) const;
    Q_SIGNAL void FFTSize(unsigned) const;
    Q_SIGNAL void detectorClose() const;
    Q_SIGNAL void finished() const;
    Q_SIGNAL void transmitFrequency(double) const;
    Q_SIGNAL void endTransmitMessage(bool quick = false) const;
    Q_SIGNAL void tune(bool = true) const;
    Q_SIGNAL void sendMessage(double frequency, int submode, double txDelay,
                              SoundOutput *, AudioDevice::Channel) const;
    Q_SIGNAL void outAttenuationChanged(qreal) const;
    Q_SIGNAL void toggleShorthand() const;
    Q_SIGNAL void submodeChanged(Varicode::SubmodeType) const;

    Q_SIGNAL void messageAdded(int) const;

  private:
    QByteArray wisdomFileName() const;

    void writeAllTxt(QStringView message);
    void writeMsgTxt(QStringView message, int snr, int offset);

    void currentTextChanged();
    void tableSelectionChanged(QItemSelection const &, QItemSelection const &);
    void setupJS8();
    void applyPillSettings();

    int freq() const { return m_freq; }

    /** Sabotage transmission if frequency would result in WSPR band. */
    void refuseToSendIn30mWSPRBand();

    void prepareSending(qint64 nowMS);

    /** Update the clock shown. */
    void updateClockUI(const QDateTime &);

    void setFreq(int);
    void transmit();

    bool presentlyWantHBReplies();

    QString m_nextFreeTextMsg;

    NetworkAccessManager m_network_manager;
    bool m_valid;
    [[maybe_unused]] bool m_multiple; // Used only in Windows builds
    MultiSettings *m_multi_settings;
    QPushButton *m_configurations_button;
    QSettings *m_settings;
    bool m_settings_read;
    QScopedPointer<Ui::UI_Constructor> ui;

    // other windows
    Configuration m_config;
    JS8MessageBox m_rigErrorMessageBox;

    QDockWidget* messageDock_ = nullptr;
    MessagePanel* messagePanel_ = nullptr;
    QDialog* messageFloatDialog_ = nullptr;
    QLineEdit* m_inboxFilterEdit_ = nullptr;

    enum class MessageHost { Dock, Dialog };
    MessageHost messageHost_ = MessageHost::Dock;

    QScopedPointer<WideGraph> m_wideGraph;
    QScopedPointer<LogQSO> m_logDlg;
    QScopedPointer<HelpTextWindow> m_shortcuts;
    QScopedPointer<HelpTextWindow> m_prefixes;
    QScopedPointer<HelpTextWindow> m_mouseCmnds;

    Transceiver::TransceiverState m_rigState;
    Frequency m_lastDialFreq;
    QString m_lastBand;

    // per-band persistent activity storage (activity.db3) - issue #267
    std::unique_ptr<ActivityDB> m_activityDB;
    QElapsedTimer m_activityDBRetryTimer; // throttles reopen attempts
    // The storage bucket the on-screen call activity / RX text belongs
    // to: a band name, or "" - the out-of-plan bucket, which persists
    // activity heard on dials outside the band plan (transverter IFs,
    // channelized operation) the way the legacy un-banded ini did.
    // Usually this equals m_lastBand, but at startup the panes are
    // preloaded from the last-known dial before the rig has reported
    // one, and that data must be flushed to - and cleared as - the
    // bucket it was loaded from, not whatever the rig later reports.
    QString m_activityBand;
    bool m_activityBandLoaded = false; // false until the first restore
    // Buckets whose stored rows have been seeded into the session's RAM
    // caches. Seeding happens once per bucket per session; until it has
    // succeeded, RX-text saves for that bucket are suppressed - the
    // on-screen document does not contain the stored history, so saving
    // it would overwrite (or, empty, delete) that history.
    QSet<QString> m_activitySeeded;
    // Degraded start: the store was down, so readSettings showed the
    // legacy ini copy in the RX pane. That copy is history the one-time
    // import already banked - only text BELOW it is session text. These
    // mark which bucket shows it and how many blocks it spans, so the
    // recovery seed and the close-time sweep never merge a second copy
    // of the history behind the stored one.
    QString m_rxTextLegacyBand;
    int m_rxTextLegacyBlocks = 0;
    // Depth counter so nested write bursts (a decode drain that calls
    // refreshInboxCounts, say) share one transaction.
    int m_activityBatchDepth = 0;
    // Bands whose latest RX text never reached the store (a flush failed
    // while the store was down): restores re-arm their saves and
    // closeEvent sweeps the stragglers from the RAM band cache.
    QSet<QString> m_rxTextDirtyBands;
    // Set when a requested startup reset could not be applied: nothing
    // is read or written for the rest of the session, so the panes stay
    // as empty as the user asked and no write of this session can be
    // destroyed by the wipe that retries at the next start.
    bool m_activityStoreDisabled = false;
    // Set in closeEvent: suppresses the seed retry that a final flush
    // would otherwise trigger while the window is tearing down.
    bool m_activityShuttingDown = false;
    // False until the rig has actually reported a band (or the startup
    // grace period lapses, covering Rig=None). While false the storage
    // bucket is only the last session's guess, so RX text - which has no
    // per-line frequency - must not be filed under it.
    bool m_activityBandConfirmed = false;
    // Buckets the rig has actually reported this session. The close-time
    // sweep writes only these: a bucket that was never confirmed holds a
    // pane that may contain text heard on a different band.
    QSet<QString> m_activityBandConfirmedBands;
    QElapsedTimer m_activityStartupTimer;
    QElapsedTimer m_seedRetryTimer; // throttles failed-seed retries
    QTimer m_rxTextSaveTimer; // debounces RX-text writes to activity.db3
    // Cap on the debounce: sustained sub-interval document changes (busy
    // nets, fast submodes) restart m_rxTextSaveTimer indefinitely, so this
    // timer - armed once and not restarted - bounds how stale the stored
    // copy can get.
    QTimer m_rxTextSaveMaxTimer;
    // Storage id captured at first use: MultiSettings updates its
    // current configuration BEFORE this window closes during a
    // configuration switch, so reading it at close time would file the
    // outgoing configuration's activity under the incoming one.
    mutable QString m_activityConfigId;
    // Skip unchanged RX-text rewrites via the document's revision
    // counter - unlike hashing toHtml(), checking it costs nothing, so
    // an idle debounce tick never serializes the (unbounded) document.
    int m_rxTextLastSavedRevision = -1;
    QString m_rxTextLastSavedBand;

    Detector *m_detector;
    unsigned m_FFTSize;
    SoundInput *m_soundInput;
    Modulator *m_modulator;
    SoundOutput *m_soundOutput;
    NotificationAudio *m_notification;

    // Configuration might one day offer to send a txDelayChanged signal.
    // As long as it doesn't, we poll and compare with the previous value.
    double m_TxDelay; // in seconds.

    TxLoop *m_cq_loop;
    TxLoop *m_hb_loop;

    QThread m_networkThread;
    QThread m_audioThread;
    QThread m_notificationAudioThread;
    JS8::Decoder m_decoder;

    qint64 m_secBandChanged;

    Frequency m_freqNominal;
    Frequency m_freqTxNominal;

    int m_freq;

    qint32 m_XIT;
    qint32 m_sec0;
    qint32 m_RxLog;
    qint32 m_nutc0;
    // The period of the current submode, in seconds. (15 for normal, 10 for
    // fast, ...)
    qint32 m_TRperiod;
    qint32 m_inGain;
    qint32 m_idleMinutes;
    qint32 m_nSubMode;
    FrequencyList_v3::const_iterator m_frequency_list_fcal_iter;
    qint32 m_i3bit;

    bool m_btxok; // True if OK to transmit
    bool m_decoderBusy;
    QString m_decoderBusyBand;
    QMap<qint32, qint32>
        m_lastDecodeStartMap; // submode, decode k start position
    QMap<qint32, qint32>
        m_lastDecodeCycleMap; // JS8 40 & JS8 60 submodes, last enqueued cycle start
    Radio::Frequency m_decoderBusyFreq;
    QDateTime m_decoderBusyStartTime;
    bool m_auto;
    bool m_restart;
    bool m_bDecoded;
    int m_currentMessageType;
    QString m_currentMessage;
    int m_currentMessageBits;
    int m_lastMessageType;
    QString m_lastMessageSent;
    bool m_tuneup;
    bool m_isTimeToSend;

    int m_ihsym;
    float m_px;
    float m_pxmax;
    float m_df3;
    quint32 m_iptt = 0;
    quint32 m_iptt0;
    bool m_btxok0;
    double m_onAirFreq0;
    bool m_first_error;

    char m_msg[100][80];

    // labels and widgets in status and header bar
    QLabel tx_status_label;
    QLabel config_label;
    QLabel mode_label;
    QLabel frequency_label;
    QLabel auto_reply_label;
    QLabel last_tx_label;
    QLabel auto_tx_label;
    QProgressBar progressBar;
    QLabel wpm_label;
    Styles::OffsetSliderWidget *freqOffsetWidget = nullptr;
    int m_sliderFreqBeforeHB = 0;

    // QPointer<QProcess> proc_js8;

    QTimer m_guiTimer;
    // Timer to switch off PTT after end of transmission.
    QTimer pttReleaseTimer;
    QTimer logQSOTimer;
    QTimer tuneButtonTimer;
    QTimer tuneATU_Timer;
    QTimer TxAgainTimer;
    QTimer minuteTimer;
    QTimer m_msgNotifyTimer;
    QString m_baseCall;
    QString m_hisCall;
    QString m_hisGrid;
    QString m_appDir;
    QString m_palette;
    QString m_rptSent;
    QString m_rptRcvd;
    QString m_msgSent0;
    QString m_opCall;

    struct CallDetail {
        QString call;
        QString through;
        QString grid;
        Frequency dial;
        int offset;
        QDateTime cqTimestamp;
        QDateTime ackTimestamp;
        QDateTime utcTimestamp;
        int snr;
        int bits;
        float tdrift;
        int submode;
    };

    struct CommandDetail {
        bool isCompound;
        bool isBuffered;
        QString from;
        QString to;
        QString cmd;
        Frequency dial;
        int offset;
        QDateTime utcTimestamp;
        int snr;
        int bits;
        QString grid;
        QString text;
        QString extra;
        float tdrift;
        int submode;
        QString relayPath;
    };

    struct ActivityDetail {
        bool isLowConfidence;
        bool isCompound;
        bool isDirected;
        bool isBuffered;
        int bits;
        Frequency dial;
        int offset;
        QString text;
        QDateTime utcTimestamp;
        int snr;
        bool shouldDisplay;
        float tdrift;
        int submode;
    };

    struct MessageBuffer {
        CommandDetail cmd;
        QQueue<CallDetail> compound;
        QList<ActivityDetail> msgs;
    };

    QString m_prevSelectedCallsign;
    int m_bandActivityWidth;
    int m_callActivityWidth;
    int m_textActivityWidth;
    int m_waterfallHeight;
    bool m_bandActivityWasVisible;
    bool m_rxDirty;
    bool m_rxDisplayDirty;
    int m_txFrameCountEstimate;
    int m_txFrameCount;
    int m_txFrameCountSent;
    QTimer m_txTextDirtyDebounce;
    bool m_txTextDirty;
    QString m_txTextDirtyLastText;
    QString m_txTextDirtyLastSelectedCall;
    QString m_lastTxMessage;
    QString m_totalTxMessage;

    // moved from mainwindow.cpp, is used in multiple functions
    QString since(QDateTime const &time) {
        auto const delta = time.secsTo(DriftingDateTime::currentDateTimeUtc());

        if (delta >= 60 * 60 * 24)
            return QString("%1d").arg(delta / (60 * 60 * 24));
        else if (delta >= 60 * 60)
            return QString("%1h").arg(delta / (60 * 60));
        else if (delta >= 60)
            return QString("%1m").arg(delta / (60));
        else if (delta >= 15)
            return QString("%1s").arg(delta - (delta % 15));
        else
            return QString("now");
    }

    QDateTime m_lastTxStartTime;
    QDateTime m_lastTxStopTime;
    qint32 m_driftMsMMA;
    qint32 m_driftMsMMA_N;

    // moved from mainwindow.cpp, is used in multiple functions
    auto replaceMacros(QString const &text,
                       QMap<QString, QString> const &values, bool const prune) {
        QString output = text;

        for (auto const [key, value] : values.asKeyValueRange()) {
            output = output.replace(key, value.toUpper());
        }

        return prune ? output.replace(QRegularExpression("[<](?:[^>]+)[>]"), "")
                     : output;
    }

    QList<int> generateOffsets(int minOffset, int maxOffset) {
        QList<int> offsets;

        // TODO: these offsets aren't ordered correctly...

        for (int i = minOffset; i <= maxOffset; i++) {
            offsets.append(i);
        }
        return offsets;
    }

    enum Priority {
        PriorityLow = 10,
        PriorityNormal = 100,
        PriorityHigh = 1000
    };

    struct PrioritizedMessage {
        QDateTime date;
        int priority;
        QString message;
        int offset;
        Callback callback;

        friend bool operator<(PrioritizedMessage const &a,
                              PrioritizedMessage const &b) {
            if (a.priority < b.priority) {
                return true;
            }
            return a.date < b.date;
        }
    };

    struct CachedDirectedType {
        bool isAllcall;
        QDateTime date;
    };

    struct DecodeParams {
        int submode;
        int start;
        int sz;
    };

    struct FrameCacheKey {
        int submode;
        QString frame;

        FrameCacheKey(int submode, QString frame)
            : submode(submode), frame(std::move(frame)) {}

        bool operator==(FrameCacheKey const &) const noexcept = default;

        struct Hash {
            std::size_t operator()(FrameCacheKey const &key) const noexcept {
                std::size_t const h1 = std::hash<int>{}(key.submode);
                std::size_t const h2 = qHash(key.frame);
                return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
            }
        };
    };

    using FrameCache =
        std::unordered_map<FrameCacheKey, QDateTime, FrameCacheKey::Hash>;
    using BandActivity = QMap<int, QList<ActivityDetail>>;

    QQueue<DecodeParams> m_decoderQueue;
    FrameCache m_messageDupeCache;  // submode, frame -> date seen
    QVariantMap m_showColumnsCache; // table column:key -> show boolean
    QVariantMap m_sortCache;        // table key -> sort by
    QPriorityQueue<PrioritizedMessage> m_txMessageQueue; // messages to be sent
    QQueue<QPair<QString, int>> m_txFrameQueue;          // frames to be sent
    QQueue<ActivityDetail> m_rxActivityQueue; // all rx activity queue
    QQueue<CommandDetail>
        m_rxCommandQueue; // command queue for processing commands
    QQueue<CallDetail>
        m_rxCallQueue; // call detail queue for spots to pskreporter
    QMap<QString, QString>
        m_compoundCallCache; // base callsign -> compound callsign
    QCache<QString, QDateTime> m_txAllcallCommandCache; // callsign -> last tx
    QCache<int, QDateTime> m_rxRecentCache;             // freq -> last rx
    QCache<int, CachedDirectedType>
        m_rxDirectedCache;                // freq -> last directed rx
    QCache<QString, int> m_rxCallCache;   // call -> last freq seen
    QMap<int, int> m_rxFrameBlockNumbers; // freq -> block
    BandActivity m_bandActivity; // freq -> [(text, last timestamp), ...]
    QMap<int, MessageBuffer> m_messageBuffer; // freq -> (cmd, [frames, ...])
    int m_lastClosedMessageBufferOffset;
    QMap<QString, CallDetail>
        m_callActivity; // call -> (last freq, last timestamp)

    QMap<int, QString> m_origRxHeaderLabelMap;           // colIndex, label
    QMap<int, QString> m_origCallActivityHeaderLabelMap; // colIndex, label
    QMap<QString, QString> m_columnLabelMap;             // full, minimal

    QMap<QString, QSet<QString>>
        m_heardGraphOutgoing; // callsign -> [stations who've this callsign has
                              // heard]
    QMap<QString, QSet<QString>>
        m_heardGraphIncoming; // callsign -> [stations who've heard this
                              // callsign]

    QMap<QString, int> m_rxInboxCountCache; // call -> count

    // The RAM band caches remain the session's display layer, exactly as
    // on master: band round-trips restore the panes verbatim, with or
    // without a working store. activity.db3 sits underneath as a
    // write-through store plus a once-per-session seed of each bucket's
    // history (see seedActivityForBand).
    QMap<QString, QMap<QString, CallDetail>>
        m_callActivityBandCache; // bucket -> call activity
    QMap<QString, QString> m_rxTextBandCache; // bucket -> rx text
    QMap<QString, QMap<int, QList<ActivityDetail>>>
        m_bandActivityBandCache;              // band -> band activity
    QMap<QString, QMap<QString, QSet<QString>>>
        m_heardGraphOutgoingBandCache; // band -> heard in
    QMap<QString, QMap<QString, QSet<QString>>>
        m_heardGraphIncomingBandCache; // band -> heard out

    // Pending autoreply confirmations (network API and local UI notification)
    struct PendingConfirmation {
        int id;
        int priority;
        QString message;
        int offset;
        Callback callback;
        QTimer *timer;
        SelfDestructMessageBox *box;
    };
    int m_nextConfirmId = 0;
    QMap<int, PendingConfirmation> m_pendingConfirmations;

    QMap<QString, QDateTime>
        m_callSelectedTime; // call -> timestamp when callsign was last selected
    /**
     * Cache of recently seen APRS relay keys used to suppress duplicates.
     * Key format: "TO|TEXT|SENDER" (uppercased), value is last seen UTC.
     */
    QHash<QString, QDateTime> m_aprsRelayDedupCache;
    QSet<QString> m_callSeenHeartbeat; // call
    bool m_bandHopped;
    Frequency m_bandHoppedFreq;

    /** Repeat period of HBs, in seconds. */
    int m_hbInterval;
    /** Repeat period of CQ calls, in seconds. */
    int m_cqInterval;

    /** Whether to resume HBs at the next opportunity. */
    bool m_hbPaused;

    QDateTime m_dateTimeQSOOn;
    QDateTime m_dateTimeLastTX;
    
    QSet<int> m_rxBlockedOffsets; //callsign blocking

    LogBook m_logBook;
    unsigned m_msAudioOutputBuffered;
    unsigned m_framesAudioInputBuffered;
    QThread::Priority m_audioThreadPriority;
    QThread::Priority m_notificationAudioThreadPriority;
    QThread::Priority m_decoderThreadPriority;
    QThread::Priority m_networkThreadPriority;
    bool m_splitMode;
    bool m_monitoring;
    bool m_generateAudioWhenPttConfirmedByTX;
    bool m_transmitting;
    bool m_tune;
    bool m_tx_watchdog; // true when watchdog triggered
    bool m_block_pwr_tooltip;
    bool m_PwrBandSetOK;
    Frequency m_lastMonitoredFrequency;
    MessageClient *m_messageClient;
    MessageServer *m_messageServer;
    WSJTXMessageClient *m_wsjtxMessageClient;
    WSJTXMessageMapper *m_wsjtxMessageMapper;
    TCPClient *m_n3fjpClient;
    PSKReporter *m_pskReporter;
    SpotClient *m_spotClient;
    APRSISClient *m_aprsClient;
    AprsInboundRelay *m_aprsInboundRelay;
    QVariantHash m_pwrBandTxMemory; // Remembers power level by band
    QVariantHash
        m_pwrBandTuneMemory; // Remembers power level by band for tuning
    QByteArray m_geometryNoControls;

    //---------------------------------------------------- private functions
    void readSettings();
    void set_application_font(QFont const &);
    void writeSettings();
    void createStatusBar();
    void statusChanged();
    void rigFailure(QString const &reason);
    void spotSetLocal();
    void pskSetLocal();
    void aprsSetLocal();
    void spotReport(int submode, Frequency dial, int offset, int snr,
                    QString const &callsign, QString const &grid);
    void spotCmd(CommandDetail const &cmd);
    void spotAprsCmd(CommandDetail const &cmd);
    void pskLogReport(QString const &mode, Frequency dial, int offset, int snr,
                      QString const &callsign, QString const &grid,
                      QDateTime const &utcTimestamp);
    void spotAprsGrid(Frequency dial, int offset, int snr, QString callsign,
                      QString grid);
    Radio::Frequency dialFrequency();
    void setSubmode(int submode);
    void updateCurrentBand();
    void displayDialFrequency();
    void transmitDisplay(bool);
    void postDecode(bool is_new, QString const &message);
    void displayTransmit();
    void updateModeButtonText();
    void updateButtonDisplay();
    void updateTextDisplay();
    void updateTextWordCheckerDisplay();
    void updateTextStatsDisplay(QString text, int count);
    void updateTxButtonDisplay();
    bool isMyCallIncluded(QString const &text);
    bool isAllCallIncluded(QString const &text);
    bool isGroupCallIncluded(const QString &text);
    QString callsignSelected(bool useInputText = false);
    void callsignSelectedChanged(QString old, QString current);
    bool isRecentOffset(int submode, int offset);
    void markOffsetRecent(int offset);
    bool isDirectedOffset(int offset, bool *pIsAllCall);
    void markOffsetDirected(int offset, bool isAllCall);
    void clearOffsetDirected(int offset);
    void processActivity(bool force = false);
    void resetTimeDeltaAverage();
    void processRxActivity(); // JS8_Mainwindow/processRxActivity.cpp
    void processIdleActivity();
    void processCompoundActivity();
    void processBufferedActivity(); // JS8_Mainwindow/processBufferedActivity.cpp
    void processCommandActivity(); // JS8_Mainwindow/processCommandActivity.cpp
    void processHeartbeatRateLimit(const QString &callsign); // JS8_Mainwindow/processHeartbeatRateLimit.cpp
    QString inboxPath();
    QString hbBlockingPath() const;
    void pushNotificationHandler(); // JS8_Mainwindow/pushNotificationHandler.cpp
    QString msgNotifyPath() const;

    // per-band persistent activity storage (activity.db3) - issue #267
    QString activityPath() const;
    ActivityDB *activityDB();
    QString activityConfigId() const;
    void persistCallActivity(CallDetail const &,
                             bool fallbackToCurrentBand = false);
    // single conversion pair, so a CallDetail field added later fails to
    // persist/restore loudly at the one site instead of silently in
    // hand-copied blocks
    ActivityDB::CallRecord toCallRecord(CallDetail const &) const;
    CallDetail fromCallRecord(ActivityDB::CallRecord const &) const;
    void seedActivityForBand(QString const &band);
    QString htmlBelowLegacyCopy(QTextDocument *doc) const;
    void purgeLegacyActivityIni();
    void beginActivityBatch();
    void startActivityBatchIfPending();
    void endActivityBatch();
    void saveRxTextForBand(QString const &band);
    bool importLegacyActivityIfNeeded();
    void removeStoredMessageNotification(int msgId);
    void refreshInboxCounts();
    bool hasMessageHistory(QString call);
    int addCommandToMyInbox(CommandDetail d);
    int addCommandToStorage(QString type, CommandDetail d);
    int getNextMessageIdForCallsign(QString callsign);
    int getLookaheadMessageIdForCallsign(QString callsign, int afterMsgId);
    int getNextGroupMessageIdForCallsign(QString group_name, QString callsign);
    int getLookaheadGroupMessageIdForCallsign(QString group_name,
                                              QString callsign, int afterMsgId);
    int countUnreadForCallsign(const QString &callsign);
    int countGroupUnreadForCallsign(const QString &group_name,
                                    const QString &callsign);
    bool markGroupMsgDeliveredForCallsign(int msgId, QString callsign);
    bool markMsgDelivered(int mid, Message msg);
    QStringList parseRelayPathCallsigns(QString from, QString text);
    void processSpots();
    void processTxQueue();
    void displayActivity(bool force = false);
    void displayBandActivity(); // JS8_Mainwindow/displayBandActivity.cpp
    void displayCallActivity(); // JS8_Mainwindow/displayCallActivity.cpp
    void enable_DXCC_entity(bool on);
    void setRig(Frequency = 0); // zero frequency means no change
    QDateTime nextTransmitCycle();
    void statusUpdate();
    void on_the_minute();
    void tryBandHop();
    void add_child_to_event_filter(QObject *);
    void remove_child_from_event_filter(QObject *);
    void setup_status_bar();
    QString columnLabel(QString defaultLabel);
    void ensureMessageDock();

    void resetIdleTimer();
    void incrementIdleTimer();
    void tx_watchdog(bool triggered);
    void write_frequency_entry(QString const &file_name);
    void write_transmit_entry(QString const &file_name);
};

#endif // MAINWINDOW_H

