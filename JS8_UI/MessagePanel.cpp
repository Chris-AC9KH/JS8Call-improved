#include "MessagePanel.h"
#include "JS8_Include/EventFilter.h"
#include "JS8_Main/Radio.h"
#include "JS8_Main/Varicode.h"
#include "JS8_Widgets/DateTableWidgetItem.h"
#include "JS8_Widgets/SemiSortableHeader.h"
#include "ui_MessagePanel.h"

#include <QAction>
#include <QDateTime>
#include <QMenu>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>

#include "moc_MessagePanel.cpp"

namespace {
auto pathSegs(QString const &path) {
  auto segs = path.split('>');
  std::reverse(segs.begin(), segs.end());
  return segs;
}
} // namespace

MessagePanel::MessagePanel(QString inboxPath, QWidget *parent)
    : QWidget(parent), ui(new Ui::MessagePanel), inbox(new Inbox(inboxPath)) {
  this->call = "%";
  inbox->open();

  ui->setupUi(this);

  replyMenu = new QMenu(ui->replyPushButton);
  replyToPathAction = replyMenu->addAction(QString());
  replyToOriginalViaPathAction = replyMenu->addAction(QString());

  ui->replyPushButton->setPopupMode(QToolButton::MenuButtonPopup);
  ui->replyPushButton->setDefaultAction(replyToPathAction);
  connect(replyToPathAction, &QAction::triggered, this, [this]() { emitReplyAction(replyToPathAction); });
  connect(replyToOriginalViaPathAction, &QAction::triggered, this, [this]() { emitReplyAction(replyToOriginalViaPathAction); });
  resetReplyButton();

  // connect selection model changed
  connect(ui->messageTableWidget->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &MessagePanel::messageTableSelectionChanged);

  // reply when key pressed in the reply box
  ui->replytextEdit->installEventFilter(new EventFilter::EnterKeyPress(
      [this](QKeyEvent *const event) {
        if (event->modifiers() & Qt::ShiftModifier)
          return false;
        ui->replyPushButton->click();
        return true;
      },
      this));

  ui->messageTableWidget->setContextMenuPolicy(Qt::ActionsContextMenu);
  auto deleteAction = new QAction("Delete", ui->messageTableWidget);
  deleteAction->setShortcut(QKeySequence(Qt::Key_Delete));
  deleteAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
  connect(deleteAction, &QAction::triggered, this,
          [this]() { deleteSelectedMessages(); });
  ui->messageTableWidget->addAction(deleteAction);

  auto *table = ui->messageTableWidget;

  auto *hdr = new SemiSortableHeader(Qt::Horizontal, table);
  hdr->addNonSortableColumn(6);
  hdr->addNonSortableColumn(0);
  hdr->setSectionResizeMode(QHeaderView::Interactive);
  hdr->setStretchLastSection(true);
  hdr->attachTo(table);

  // Now that the custom header is installed; indicator state will stick
  {
    QSignalBlocker b(hdr);
    hdr->setSortIndicator(1, Qt::DescendingOrder);
  }

  refresh();
}

MessagePanel::~MessagePanel() { delete ui; }

void MessagePanel::setCall(const QString &call) {
  this->call = call;
  setWindowTitle(QString("Messages: %1").arg(call == "%" ? "All" : call));
  if (!inbox)
    return;

  refresh();
}

void MessagePanel::refresh() {
  QList<QPair<int, Message>> msgs = inbox->fetchForCall(this->call);
  populateMessages(msgs);

  emit countsUpdated();
}

void MessagePanel::populateMessages(QList<QPair<int, Message>> msgs) {
  SemiSortableHeader *hdr = static_cast<SemiSortableHeader *>(
      ui->messageTableWidget->horizontalHeader());

  // Remember sort state (avoid sorting by unread flag column 0)
  const int rememberedCol = hdr->sortIndicatorSection() >= 0 ? hdr->sortIndicatorSection() : 1;
  const int sortCol = (rememberedCol == 0 ? 1 : rememberedCol);
  const Qt::SortOrder sortOrder = hdr->sortIndicatorOrder();

  // Freeze behavior while populating
  ui->messageTableWidget->setUpdatesEnabled(false);
  const bool wasSorting = ui->messageTableWidget->isSortingEnabled();
  ui->messageTableWidget->setSortingEnabled(false);

  for (int i = ui->messageTableWidget->rowCount() - 1; i >= 0; --i) {
    ui->messageTableWidget->removeRow(i);
  }

  {
    foreach (auto pair, msgs) {
      auto mid = pair.first;
      auto msg = pair.second;
      auto params = msg.params();

      int row = ui->messageTableWidget->rowCount();
      ui->messageTableWidget->insertRow(row);

      int col = 0;

      auto typeItem =
          new QTableWidgetItem(msg.type() == "UNREAD" ? "\u2691" : "");
      typeItem->setData(Qt::UserRole, msg.type());
      typeItem->setTextAlignment(Qt::AlignCenter);
      ui->messageTableWidget->setItem(row, col++, typeItem);

      auto midItem = new QTableWidgetItem();
      midItem->setData(Qt::EditRole, mid);
      midItem->setData(Qt::DisplayRole, QString::number(mid));
      midItem->setTextAlignment(Qt::AlignCenter);
      ui->messageTableWidget->setItem(row, col++, midItem);

      auto path = params.value("PATH").toString();
      auto segs = pathSegs(path);
      auto fromItem = new QTableWidgetItem(segs.join(" via "));
      fromItem->setData(Qt::UserRole, path);
      fromItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      ui->messageTableWidget->setItem(row, col++, fromItem);

      auto to = params.value("TO").toString();
      auto toItem = new QTableWidgetItem(to);
      toItem->setData(Qt::UserRole, to);
      toItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      ui->messageTableWidget->setItem(row, col++, toItem);

      const auto dateStr = params.value("UTC").toString();
      QDateTime ts = QDateTime::fromString(dateStr, "yyyy-MM-dd HH:mm:ss");
      ts.setTimeZone(QTimeZone::utc());

      auto *dateItem = new DateItem(ts.toString("ddd MMM d HH:mm:ss yyyy"));
      dateItem->setData(Qt::UserRole, ts.toSecsSinceEpoch());   // sort key
      dateItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      ui->messageTableWidget->setItem(row, col++, dateItem);

      auto dial = (quint64)params.value("DIAL").toInt();
      auto dialItem = new QTableWidgetItem();
      dialItem->setData(Qt::EditRole, dial);
      dialItem->setData(Qt::DisplayRole, QString("%1 MHz").arg(Radio::pretty_frequency_MHz_string(dial)));
      dialItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
      ui->messageTableWidget->setItem(row, col++, dialItem);

      auto text = params.value("TEXT").toString();
      auto textItem = new QTableWidgetItem(text);
      textItem->setData(Qt::UserRole, text);
      textItem->setTextAlignment(Qt::AlignVCenter);
      ui->messageTableWidget->setItem(row, col++, textItem);
    }
  }

  if (wasSorting) {
    ui->messageTableWidget->sortItems(sortCol, sortOrder);
  }

  ui->messageTableWidget->resizeColumnToContents(0);
  ui->messageTableWidget->resizeColumnToContents(1);
  ui->messageTableWidget->resizeColumnToContents(2);
  ui->messageTableWidget->resizeColumnToContents(3);
  ui->messageTableWidget->resizeColumnToContents(4);
  ui->messageTableWidget->resizeColumnToContents(5);

  ui->messageTableWidget->setSortingEnabled(wasSorting);     // or true if you want arrows always
  ui->messageTableWidget->setUpdatesEnabled(true);

  if (hdr) {
    QSignalBlocker b(hdr);
    hdr->setSortIndicator(sortCol, sortOrder);
  }

  ui->messageTableWidget->viewport()->update();

  emit countsUpdated();
}

void MessagePanel::deleteSelectedMessages() {
  auto items = ui->messageTableWidget->selectedItems();
  if (items.isEmpty()) {
    return;
  }

  // Collect unique rows and their message IDs
  QMap<int, int> rowsToDelete; // row -> message id
  for (auto item : items) {
    int row = item->row();
    if (rowsToDelete.contains(row)) {
      continue;
    }

    auto col = ui->messageTableWidget->item(row, 1);
    if (!col) {
      continue;
    }

    bool ok = false;
    auto mid = col->data(Qt::EditRole).toInt(&ok);
    if (!ok) {
      continue;
    }

    rowsToDelete.insert(row, mid);
  }

  // Delete rows in reverse order to avoid index shifting
  auto rows = rowsToDelete.keys();
  std::sort(rows.begin(), rows.end(), std::greater<int>());

  for (int row : rows) {
    ui->messageTableWidget->removeRow(row);
    deleteMessage(rowsToDelete[row]);
  }

  emit countsUpdated();
}

void MessagePanel::deleteMessage(int id) {
  if (!inbox->open()) {
    return;
  }

  inbox->del(id);
}

void MessagePanel::markMessageRead(int id) {
  if (!inbox->open()) {
    return;
  }

  auto msg = inbox->value(id);

  if (msg.type() == "UNREAD") {
    msg.setType("READ");
    inbox->set(id, msg);

    // Also clear the unread flag in the table UI for this message id
    auto *table = ui->messageTableWidget;
    if (table) {
      for (int row = 0; row < table->rowCount(); ++row) {
        auto *idItem = table->item(row, 1); // column 1 holds the message id
        if (!idItem)
          continue;
        bool okId = false;
        int midInRow = idItem->data(Qt::EditRole).toInt(&okId);
        if (!okId)
          continue;
        if (midInRow == id) {
          auto *flagItem = table->item(row, 0); // column 0 is the unread flag
          if (flagItem) {
            flagItem->setText("");
            flagItem->setData(Qt::UserRole, "");
          }
          break;
        }
      }
    }

    emit countsUpdated();
  }
}

QString MessagePanel::prepareReplyMessage(QString path, QString text) {
  return QString("%1 MSG %2").arg(path).arg(text);
}

QString MessagePanel::prepareStoreForwardReplyMessage(QString path,
                                                      QString recipient,
                                                      QString text) {
  return QString("%1 MSG TO:%2 %3").arg(path).arg(recipient).arg(text);
}

void MessagePanel::resetReplyButton() {
  ui->replyPushButton->setMenu(nullptr);

  replyToPathAction->setText(tr("Reply"));
  replyToPathAction->setData(QVariant());
  replyToPathAction->setEnabled(false);

  replyToOriginalViaPathAction->setText(QString());
  replyToOriginalViaPathAction->setData(QVariant());
  replyToOriginalViaPathAction->setEnabled(false);
}

void MessagePanel::configureReplyButton(int row, const QString &messageText) {

  auto *pathItem = ui->messageTableWidget->item(
      row, ui->messageTableWidget->columnCount() - 5);
  if (!pathItem) {
    return;
  }

  const auto path = pathItem->data(Qt::UserRole).toString().trimmed();
  if (path.isEmpty()) {
    return;
  }

  const auto placeholder = QStringLiteral("[MESSAGE]");
  replyToPathAction->setText(tr("Reply to %1").arg(path));
  replyToPathAction->setData(prepareReplyMessage(path, placeholder));
  replyToPathAction->setEnabled(true);

  static const QRegularExpression fromSignature(R"((?:^| )FROM (?<callsign>\S+)$)");
  const auto match = fromSignature.match(messageText);
  if (!match.hasMatch()) {
    return;
  }

  const auto originalSender = match.captured("callsign");
  if (originalSender.startsWith('@') || !Varicode::isValidCallsign(originalSender, nullptr)) {
    return;
  }

  replyToOriginalViaPathAction->setText(tr("Reply to %1 via %2").arg(originalSender, path));
  replyToOriginalViaPathAction->setData(prepareStoreForwardReplyMessage(path, originalSender, placeholder));
  replyToOriginalViaPathAction->setEnabled(true);
  ui->replyPushButton->setMenu(replyMenu);
}

void MessagePanel::emitReplyAction(QAction *action) {
  const auto message = action->data().toString();
  if (!message.isEmpty()) {
    emit replyMessage(message);
  }
}

void MessagePanel::messageTableSelectionChanged(
    const QItemSelection &selected,
    const QItemSelection &/*deselected*/) {

  resetReplyButton();

  auto items = ui->messageTableWidget->selectedItems();
  if (items.isEmpty() || items.size() > ui->messageTableWidget->columnCount()) {
    ui->messageTextEdit->clear();
    return;
  }

  auto firstItem = items.first();

  auto row = firstItem->row();

  // message column
  auto item = ui->messageTableWidget->item(
      row, ui->messageTableWidget->columnCount() - 1);
  if (!item) {
    return;
  }

  auto text = item->data(Qt::UserRole).toString();
  ui->messageTextEdit->setPlainText(text);
  configureReplyButton(row, text);

  // Mark deselected as read in table
  auto selectedItemIndexes = selected.indexes();

  if (selectedItemIndexes.empty()) {
    return;
  }

  auto selectedRowIndex = selectedItemIndexes.first();
  auto selectedRow = selectedRowIndex.row();
  auto readFlagItem = ui->messageTableWidget->item(selectedRow, 0);
  readFlagItem->setText("");
  readFlagItem->setData(Qt::UserRole, "");

  // Mark message read in DB
  auto col = ui->messageTableWidget->item(selectedRow, 1);
  if (!col) {
    return;
  }

  bool ok = false;
  auto mid = col->data(Qt::EditRole).toInt(&ok);
  if (!ok) {
    return;
  }

  markMessageRead(mid);
}
