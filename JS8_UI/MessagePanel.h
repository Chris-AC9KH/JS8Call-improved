#ifndef JS8CALL_MESSAGEPANEL_H
#define JS8CALL_MESSAGEPANEL_H
#include "JS8_Main/Inbox.h"
#include "JS8_Main/Message.h"

#include <QItemSelection>
#include <QPair>
#include <QWidget>

class QAction;
class QMenu;

namespace Ui {
class MessagePanel;
}

class MessagePanel : public QWidget {
  Q_OBJECT
public:
  explicit MessagePanel(QString inboxPath, QWidget* parent = nullptr);
  ~MessagePanel() override;
signals:
  void replyMessage(const QString &call);
  void countsUpdated();
  void requestFloat();
  void requestDock();

public slots:
  void setCall(const QString &call);
  void refresh();
  void populateMessages(QList<QPair<int, Message>> msgs);
  QString prepareReplyMessage(QString path, QString text);

private slots:
  void messageTableSelectionChanged(const QItemSelection & /*selected*/,
                                    const QItemSelection & /*deselected*/);

private:
  void configureReplyButton(int row, const QString &messageText);
  void resetReplyButton();
  void emitReplyAction(QAction *action);
  QString prepareStoreForwardReplyMessage(QString path, QString recipient,
                                          QString text);
  void deleteSelectedMessages(); // shared by context menu + Delete key
  void deleteMessage(int id);
  void markMessageRead(int id);
  Ui::MessagePanel *ui;
  Inbox *inbox;
  QMenu *replyMenu;
  QAction *replyToPathAction;
  QAction *replyToOriginalViaPathAction;
  QString call;
};
#endif // JS8CALL_MESSAGEPANEL_H
