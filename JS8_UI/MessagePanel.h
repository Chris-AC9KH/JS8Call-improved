#ifndef JS8CALL_MESSAGEPANEL_H
#define JS8CALL_MESSAGEPANEL_H
#include "JS8_Main/Inbox.h"
#include "JS8_Main/Message.h"

#include <QItemSelection>
#include <QPair>
#include <QWidget>

namespace Ui { class MessagePanel; }

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
  void messageDeleted(int id);

public slots:
  void setCall(const QString &call);
  void refresh();
  void populateMessages(QList<QPair<int, Message>> msgs);
  QString prepareReplyMessage(QString path, QString text);

private slots:
  void messageTableSelectionChanged(const QItemSelection & /*selected*/,
                                    const QItemSelection & /*deselected*/);
  void on_replyPushButton_clicked();

private:
  void deleteSelectedMessages(); // shared by context menu + Delete key
  void deleteMessage(int id);
  void markMessageRead(int id);
  Ui::MessagePanel *ui;
  Inbox *inbox;
  QString call;
};
#endif // JS8CALL_MESSAGEPANEL_H
