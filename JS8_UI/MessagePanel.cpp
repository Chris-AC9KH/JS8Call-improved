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
#include <QStyle>
#include <QTimeZone>
#include <QToolButton>

#include <algorithm>

#include "moc_MessagePanel.cpp"

namespace {
auto pathSegs(QString const &path) {
    auto segs = path.split('>');
    std::reverse(segs.begin(), segs.end());
    return segs;
}

void setToolButtonMenu(QToolButton *button, QMenu *menu) {
    button->setPopupMode(menu ? QToolButton::MenuButtonPopup
                              : QToolButton::DelayedPopup);
    // Qt style sheets cache property-selector matches. Re-polish after changing
    // popupMode so the split-button padding rule is applied or removed now.
    button->style()->unpolish(button);
    button->style()->polish(button);

    // setMenu() invalidates QToolButton's cached size hint. Keep it after the
    // style refresh so the next hint uses the newly selected padding rule.
    button->setMenu(menu);
    button->updateGeometry();
    button->update();
}

// Named column indices for messageTableWidget, in the exact order they're
// inserted in populateMessages() below. Using these everywhere instead of
// magic numbers (or columnCount()-derived offsets) means a lookup can't
// silently start pointing at the wrong column if a column is ever added,
// removed, or reordered - it'll fail to compile or be an obvious one-line
// fix here, instead of a fragile runtime guess elsewhere in the file.
enum Column : int {
    Unread = 0,
    Id,
    Path,
    To,
    Date,
    Dial,
    Text,
    ColumnCount
};

// Resolves a table row to its underlying message id. Returns -1 if the row
// doesn't have one (e.g. out of range, or the Id item is somehow missing).
int messageIdForRow(QTableWidget *table, int row) {
    if (row < 0) {
        return -1;
    }
    auto item = table->item(row, Column::Id);
    if (!item) {
        return -1;
    }
    bool ok = false;
    auto mid = item->data(Qt::EditRole).toInt(&ok);
    return ok ? mid : -1;
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
    replyToOriginalAction = replyMenu->addAction(QString());

    ui->replyPushButton->setDefaultAction(replyToPathAction);
    connect(replyToPathAction, &QAction::triggered, this, [this]() { emitReplyAction(replyToPathAction); });
    connect(replyToOriginalViaPathAction, &QAction::triggered, this, [this]() { emitReplyAction(replyToOriginalViaPathAction); });
    connect(replyToOriginalAction, &QAction::triggered, this, [this]() { emitReplyAction(replyToOriginalAction); });
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
    hdr->addNonSortableColumn(Column::Text);
    hdr->addNonSortableColumn(Column::Unread);
    hdr->setSectionResizeMode(QHeaderView::Interactive);
    hdr->setStretchLastSection(true);
    hdr->attachTo(table);

    // Now that the custom header is installed; indicator state will stick
    {
        QSignalBlocker b(hdr);
        hdr->setSortIndicator(Column::Id, Qt::DescendingOrder);
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

    // Remember sort state (avoid sorting by unread flag column)
    const int rememberedCol = hdr->sortIndicatorSection() >= 0 ? hdr->sortIndicatorSection() : Column::Id;
    const int sortCol = (rememberedCol == Column::Unread ? Column::Id : rememberedCol);
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

            auto typeItem =
                    new QTableWidgetItem(msg.type() == "UNREAD" ? "\u2691" : "");
            typeItem->setData(Qt::UserRole, msg.type());
            typeItem->setTextAlignment(Qt::AlignCenter);
            ui->messageTableWidget->setItem(row, Column::Unread, typeItem);

            auto midItem = new QTableWidgetItem();
            midItem->setData(Qt::EditRole, mid);
            midItem->setData(Qt::DisplayRole, QString::number(mid));
            midItem->setTextAlignment(Qt::AlignCenter);
            ui->messageTableWidget->setItem(row, Column::Id, midItem);

            auto path = params.value("PATH").toString();
            auto segs = pathSegs(path);
            auto fromItem = new QTableWidgetItem(segs.join(" via "));
            fromItem->setData(Qt::UserRole, path);
            fromItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->messageTableWidget->setItem(row, Column::Path, fromItem);

            auto to = params.value("TO").toString();
            auto toItem = new QTableWidgetItem(to);
            toItem->setData(Qt::UserRole, to);
            toItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->messageTableWidget->setItem(row, Column::To, toItem);

            const auto dateStr = params.value("UTC").toString();
            QDateTime ts = QDateTime::fromString(dateStr, "yyyy-MM-dd HH:mm:ss");
            ts.setTimeZone(QTimeZone::utc());

            auto *dateItem = new DateItem(ts.toString("ddd MMM d HH:mm:ss yyyy"));
            dateItem->setData(Qt::UserRole, ts.toSecsSinceEpoch());   // sort key
            dateItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->messageTableWidget->setItem(row, Column::Date, dateItem);

            auto dial = params.value("DIAL").value<quint64>();
            auto dialItem = new QTableWidgetItem();
            dialItem->setData(Qt::EditRole, dial);
            dialItem->setData(Qt::DisplayRole, QString("%1 MHz").arg(Radio::pretty_frequency_MHz_string(dial)));
            dialItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            ui->messageTableWidget->setItem(row, Column::Dial, dialItem);

            auto text = params.value("TEXT").toString();
            auto textItem = new QTableWidgetItem(text);
            textItem->setData(Qt::UserRole, text);
            textItem->setTextAlignment(Qt::AlignVCenter);
            ui->messageTableWidget->setItem(row, Column::Text, textItem);
        }
    }

    if (wasSorting) {
        ui->messageTableWidget->sortItems(sortCol, sortOrder);
    }

    for (int c = 0; c < Column::Text; ++c) {
        ui->messageTableWidget->resizeColumnToContents(c);
    }

    ui->messageTableWidget->setSortingEnabled(wasSorting);     // or true if we want arrows always
    ui->messageTableWidget->setUpdatesEnabled(true);

    if (hdr) {
        QSignalBlocker b(hdr);
        hdr->setSortIndicator(sortCol, sortOrder);
    }

    ui->messageTableWidget->viewport()->update();
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

        auto mid = messageIdForRow(ui->messageTableWidget, row);
        if (mid < 0) {
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

    if (inbox->del(id)) {
        emit messageDeleted(id);
    }
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
                if (messageIdForRow(table, row) != id) {
                    continue;
                }
                auto *flagItem = table->item(row, Column::Unread);
                if (flagItem) {
                    flagItem->setText("");
                    flagItem->setData(Qt::UserRole, "");
                }
                break;
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
    setToolButtonMenu(ui->replyPushButton, nullptr);

    replyToPathAction->setText(tr("Reply"));
    replyToPathAction->setData(QVariant());
    replyToPathAction->setEnabled(false);

    replyToOriginalViaPathAction->setText(QString());
    replyToOriginalViaPathAction->setData(QVariant());
    replyToOriginalViaPathAction->setEnabled(false);
}

void MessagePanel::configureReplyButton(int row, const QString &messageText) {

    // Resolve the path by looking the message up directly in the database by
    // its id, rather than reading it back out of a table-widget column.
    auto mid = messageIdForRow(ui->messageTableWidget, row);
    if (mid < 0) {
        return;
    }

    if (!inbox->open()) {
        return;
    }

    const auto msg = inbox->value(mid);
    const auto path = msg.params().value("PATH").toString().trimmed();
    if (path.isEmpty()) {
        return;
    }

    const auto placeholder = QStringLiteral("[MESSAGE]");
    replyToPathAction->setText(tr("Reply to %1").arg(path));
    replyToPathAction->setData(prepareReplyMessage(path, placeholder));
    replyToPathAction->setEnabled(true);

    static const QRegularExpression fromSignature(R"((?:^| )FROM (?<callsign>\S+)(?: NEXT MSG ID \d+(?: \+\d+)?)?$)");
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

    replyToOriginalAction->setText(tr("Reply to %1").arg(originalSender));
    replyToOriginalAction->setData(prepareReplyMessage(originalSender, placeholder));
    replyToOriginalAction->setEnabled(true);

    setToolButtonMenu(ui->replyPushButton, replyMenu);
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

    auto item = ui->messageTableWidget->item(row, Column::Text);
    if (!item) {
        return;
    }

    auto text = item->data(Qt::UserRole).toString();
    ui->messageTextEdit->setPlainText(text);
    configureReplyButton(row, text);

    // Mark selected as read in table
    auto selectedItemIndexes = selected.indexes();

    if (selectedItemIndexes.empty()) {
        return;
    }

    auto selectedRowIndex = selectedItemIndexes.first();
    auto selectedRow = selectedRowIndex.row();
    auto readFlagItem = ui->messageTableWidget->item(selectedRow, Column::Unread);
    readFlagItem->setText("");
    readFlagItem->setData(Qt::UserRole, "");

    // Mark message read in DB
    auto mid = messageIdForRow(ui->messageTableWidget, selectedRow);
    if (mid < 0) {
        return;
    }

    markMessageRead(mid);
}

void MessagePanel::on_replyPushButton_clicked() {
    auto row = ui->messageTableWidget->currentRow();

    auto mid = messageIdForRow(ui->messageTableWidget, row);
    if (mid < 0) {
        return;
    }

    // Resolve the path by looking the message up directly in the database by
    // its id, rather than reading it back out of a table-widget column at a
    // guessed offset. The id (from the Id column above) is the only piece of
    // widget state this needs; everything else comes straight from inbox_v2,
    // which is now the actual source of truth for the message's fields.
    if (!inbox->open()) {
        return;
    }
    auto msg = inbox->value(mid);
    auto path = msg.params().value("PATH").toString();

    auto text = "[MESSAGE]";
    auto message = prepareReplyMessage(path, text);

    emit replyMessage(message);
}
