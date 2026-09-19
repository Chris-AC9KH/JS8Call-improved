/**
 * @file TransmitTextEdit.cpp
 * @brief Implementation of TransmitTextEdit class for the text entry box
 */

#include "TransmitTextEdit.h"

#include "JS8_Include/commons.h"
#include "JS8_Include/pimpl_impl.h"
#include "PillRenderer.h"
#include "TransmitTextSourceMirror.h"
#include "Varicode.h"

#include <QHelpEvent>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QSignalBlocker>
#include <QTextDocument>

#include <iterator>

Q_DECLARE_LOGGING_CATEGORY(transmittextedit_js8)

namespace {

QString normalizeText(const QString &text) {
#if JS8_ALLOW_EXTENDED
    QString normalized = text;
#else
    QString normalized = text.normalized(QString::NormalizationForm_KD);
#endif

    QString result;
    std::copy_if(normalized.begin(), normalized.end(),
                 std::back_inserter(result), [](auto const c) {
                     auto const lc = c.toLatin1();
                     return (lc && (lc == 0x10 || lc == 0x1A ||
                                    ((lc >= 32) && (lc <= 127))))
#if JS8_ALLOW_UNICODE
                            || c.isPrint();
#elif JS8_ALLOW_EXTENDED
        || Varicode::extendedChars().contains(c.toUpper())
#endif
                     ;
                 });

    return result;
}

void assertSourceMirrorSynchronized(const TransmitTextEdit *edit) {
#ifndef NDEBUG
    Q_ASSERT(normalizeText(edit->QTextEdit::toPlainText().toUpper()) ==
             edit->toPlainText());
#else
    Q_UNUSED(edit)
#endif
}

} // namespace

class ScopedDocumentMutation {
  public:
    explicit ScopedDocumentMutation(TransmitTextEdit *edit)
        : m_edit(edit), m_blocker(edit ? edit->document() : nullptr) {
        if (m_edit) {
            m_edit->beginInternalDocumentMutation();
        }
    }

    ~ScopedDocumentMutation() {
        if (m_edit) {
            m_edit->endInternalDocumentMutation();
        }
    }

  private:
    TransmitTextEdit *m_edit;
    QSignalBlocker m_blocker;
};

void setTextEditFont(QTextEdit *edit, QFont font) {
    // all uppercase
    font.setCapitalization(QFont::AllUppercase);

    edit->setFont(font);
    edit->setFontFamily(font.family());
    edit->setFontItalic(font.italic());
    edit->setFontPointSize(font.pointSize());
    edit->setFontUnderline(font.underline());
    edit->setFontWeight(font.weight());

    auto d = edit->document();
    d->setDefaultFont(font);
    edit->setDocument(d);

    auto c = edit->textCursor();
    c.select(QTextCursor::Document);
    auto cf = c.blockCharFormat();
    cf.setFont(font);
    cf.setFontCapitalization(QFont::AllUppercase);
    c.mergeBlockCharFormat(cf);

    edit->updateGeometry();
}

void setTextEditStyle(QTextEdit *edit, QColor fg, QColor bg, QFont font) {
    edit->setStyleSheet(QString("QTextEdit { color:%1; background: %2; %3; }")
                            .arg(fg.name())
                            .arg(bg.name())
                            .arg(font_as_stylesheet(font)));

    // QTimer::singleShot(10, nullptr, [edit, fg, bg](){
    QPalette p = edit->palette();
    p.setColor(QPalette::Base, bg);
    p.setColor(QPalette::Active, QPalette::Base, bg);
    p.setColor(QPalette::Disabled, QPalette::Base, bg);
    p.setColor(QPalette::Inactive, QPalette::Base, bg);

    p.setColor(QPalette::Text, fg);
    p.setColor(QPalette::Active, QPalette::Text, fg);
    p.setColor(QPalette::Disabled, QPalette::Text, fg);
    p.setColor(QPalette::Inactive, QPalette::Text, fg);

    edit->setBackgroundRole(QPalette::Base);
    edit->setForegroundRole(QPalette::Text);
    edit->setPalette(p);

    edit->updateGeometry();
    edit->update();
    //});
}

void highlightBlock(QTextBlock block, QFont font, QColor foreground,
                    QColor background) {
    Q_UNUSED(background)
    QTextCursor cursor(block);
    cursor.select(QTextCursor::BlockUnderCursor);

    auto charFormat = cursor.charFormat();
    charFormat.setFont(font);
    charFormat.setFontCapitalization(QFont::AllUppercase);
    charFormat.setForeground(QBrush(foreground));
    cursor.setCharFormat(charFormat);
}

TransmitTextEdit::TransmitTextEdit(QWidget *parent)
    : QTextEdit(parent), m_sent{0}, m_protected{false}, m_pillRenderer{nullptr},
      m_sourceMirror{this} {
    connect(this, &QTextEdit::selectionChanged, this,
            &TransmitTextEdit::on_selectionChanged);
    connect(this, &QTextEdit::cursorPositionChanged, this,
            &TransmitTextEdit::on_selectionChanged);
    connect(this->document(), &QTextDocument::contentsChange, this,
            &TransmitTextEdit::on_textContentsChanged);
    installEventFilter(this);

    // The visible document is presentation-only. Real undo/redo lives in the
    // hidden source document so pill formatting cannot pollute history.
    document()->setUndoRedoEnabled(false);

    m_pillRenderer = new PillRenderer(this, this);
}

TransmitTextEdit::~TransmitTextEdit() {}

void TransmitTextEdit::setCharsSent(int n) {
    n = qMin(n, m_sourceMirror->plainText().size());
    m_sent = n;
    m_sourceMirror->updateSentCache();
    highlight();
}

// override
QString TransmitTextEdit::toPlainText() const {
    return m_sourceMirror->plainText();
}

// override
void TransmitTextEdit::setPlainText(const QString &text) {
    m_textSent.clear();
    m_sent = 0;
    const QString normalized = normalizeText(text.toUpper());
    if (QTextEdit::toPlainText() == normalized) {
        m_sourceMirror->syncDocument(normalized);
        m_sourceMirror->updateSentCache();
        m_lastText = normalized;
        m_dirty = true;
        assertSourceMirrorSynchronized(this);
        highlight();
        return;
    }

    m_sourceMirror->syncDocument(normalized);
    m_sourceMirror->beginSuppression();
    QTextEdit::setPlainText(normalized);
    m_sourceMirror->endSuppression();
}

//
void TransmitTextEdit::replaceUnsentText(const QString &text, bool keepCursor) {
    const QString normalized = normalizeText(text.toUpper());

    m_sourceMirror->replaceUnsentText(m_sent, normalized);

    m_sourceMirror->beginSuppression();
    auto rel = relativeTextCursorPosition(textCursor());
    auto c = textCursor();
    c.beginEditBlock();
    c.movePosition(QTextCursor::Start);
    c.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, m_sent);
    c.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    c.removeSelectedText();
    c.insertText(normalized);
    c.endEditBlock();

    if (keepCursor) {
        c.movePosition(QTextCursor::End);
        c.movePosition(QTextCursor::PreviousCharacter, QTextCursor::MoveAnchor,
                       rel.first);
        c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                       rel.second - rel.first);
        setTextCursor(c);
    }
    m_sourceMirror->endSuppression();
}

//
void TransmitTextEdit::replacePlainText(const QString &text, bool keepCursor) {
    const QString normalized = normalizeText(text.toUpper());
    m_sourceMirror->syncDocument(normalized);
    m_sourceMirror->beginSuppression();
    m_sourceMirror->replaceVisiblePlainText(normalized, keepCursor);
    m_sourceMirror->endSuppression();
}

//
void TransmitTextEdit::setFont(QFont f) {
    m_font = f;

    highlight();
}

//
void TransmitTextEdit::setFont(QFont f, QColor fg, QColor bg) {
    m_font = f;
    m_fg = fg;
    m_bg = bg;

    highlight();
}

void TransmitTextEdit::undo() {
    auto *sourceDocument = m_sourceMirror->document();
    if (!sourceDocument || !sourceDocument->isUndoAvailable()) {
        return;
    }

    auto cursorState = m_sourceMirror->captureCursorState();
    sourceDocument->undo();
    m_sourceMirror->updateSentCache();
    m_lastText = m_sourceMirror->plainText();
    m_dirty = true;
    m_sourceMirror->beginSyncFromSource();
    {
        ScopedDocumentMutation guard(this);
        m_sourceMirror->syncDisplayFromSource(cursorState, true);
    }
    m_sourceMirror->endSyncFromSource();
    highlight();
}

void TransmitTextEdit::redo() {
    auto *sourceDocument = m_sourceMirror->document();
    if (!sourceDocument || !sourceDocument->isRedoAvailable()) {
        return;
    }

    auto cursorState = m_sourceMirror->captureCursorState();
    sourceDocument->redo();
    m_sourceMirror->updateSentCache();
    m_lastText = m_sourceMirror->plainText();
    m_dirty = true;
    m_sourceMirror->beginSyncFromSource();
    {
        ScopedDocumentMutation guard(this);
        m_sourceMirror->syncDisplayFromSource(cursorState, true);
    }
    m_sourceMirror->endSyncFromSource();
    highlight();
}

// override
void TransmitTextEdit::clear() {
    m_textSent.clear();
    m_sent = 0;
    if (QTextEdit::toPlainText().isEmpty()) {
        m_sourceMirror->syncDocument(QString());
        m_sourceMirror->updateSentCache();
        m_lastText.clear();
        m_dirty = true;
        assertSourceMirrorSynchronized(this);
        highlight();
        return;
    }

    m_sourceMirror->syncDocument(QString());
    m_sourceMirror->beginSuppression();
    QTextEdit::clear();
    m_sourceMirror->endSuppression();
}

void TransmitTextEdit::setProtected(bool protect) { m_protected = protect; }

bool TransmitTextEdit::cursorShouldBeProtected(QTextCursor c) {
    int start = c.selectionStart();
    int end = c.selectionEnd();
    if (end < start) {
        int x = end;
        end = start;
        start = x;
    }

    // qCDebug(transmittextedit_js8) << "selection" << start << end << m_sent;

    if (m_sent && start <= m_sent) {
        qCDebug(transmittextedit_js8)
            << "cursor in protected zone" << start << "<=" << m_sent;
        return true;
    } else {
        return false;
    }
}

// slot
void TransmitTextEdit::on_selectionChanged() {
    auto c = textCursor();

    auto shouldProtect = cursorShouldBeProtected(c);

    if (shouldProtect) {
        blockSignals(true);
        {
            int end = c.selectionEnd();
            c.movePosition(QTextCursor::Start);
            c.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor,
                           m_sent);
            c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor,
                           qMax(0, end - m_sent));
            setTextCursor(c);
        }
        blockSignals(false);
    }

    setProtected(shouldProtect);
}

// slot
void TransmitTextEdit::on_textContentsChanged(int pos, int rem, int add) {
    if (isInternalDocumentMutationActive() ||
        m_sourceMirror->isSyncingFromSource()) {
        return;
    }

    if (rem == 0 && add == 0) {
        return;
    }

    const QString visibleText = QTextEdit::toPlainText();
    const QString displayText = visibleText.toUpper();
    QString text = normalizeText(displayText);

    if (!m_sourceMirror->isSuppressing() && text == m_lastText) {
        return;
    }

    if (text != displayText) {
        {
            ScopedDocumentMutation guard(this);
            m_sourceMirror->replaceVisiblePlainText(text, true);
        }
        m_sourceMirror->syncDocument(text);
    } else if (!m_sourceMirror->isSuppressing()) {
        const int sourcePos = qBound(0, pos, m_sourceMirror->plainText().size());
        m_sourceMirror->applyChange(sourcePos, rem, displayText.mid(sourcePos, add));
    }

    if (m_sourceMirror->plainText() != text) {
        m_sourceMirror->syncDocument(text);
    }

    m_sourceMirror->updateSentCache();
    m_dirty = true;
    m_lastText = text;
    assertSourceMirrorSynchronized(this);
    highlight();
}

void TransmitTextEdit::highlightBase() {
    Q_ASSERT(isInternalDocumentMutationActive());

    auto d = document();
    if (!d) {
        return;
    }

    auto block = d->firstBlock();
    while (block.isValid()) {
        highlightBlock(block, m_font, m_fg, m_bg);
        block = block.next();
    }
}

void TransmitTextEdit::highlightCharsSent() {
    Q_ASSERT(isInternalDocumentMutationActive());

    if (!m_sent) {
        return;
    }

    auto d = document();
    if (!d) {
        return;
    }

    // highlight sent text
    auto c = textCursor();
    if (c.isNull()) {
        return;
    }
    c.movePosition(QTextCursor::Start);
    c.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor, m_sent);

    QTextCharFormat defaultFormat;
    auto ch = c.charFormat();
    ch.setFontUnderline(false);
    ch.setUnderlineStyle(QTextCharFormat::NoUnderline);
    ch.setUnderlineColor(defaultFormat.underlineColor());
    ch.setFontStrikeOut(true);
    c.mergeCharFormat(ch);
}

void TransmitTextEdit::paintEvent(QPaintEvent *event) {
    m_pillRenderer->paintPills(event);
    QTextEdit::paintEvent(event);
}

bool TransmitTextEdit::event(QEvent *e) {
    if (e->type() == QEvent::ToolTip &&
        m_pillRenderer->handleTooltip(static_cast<QHelpEvent *>(e)))
        return true;
    return QTextEdit::event(e);
}

void TransmitTextEdit::highlight() {
    if (m_isHighlighting)
        return;
    m_isHighlighting = true;

    {
        ScopedDocumentMutation guard(this);
        highlightBase();
        highlightCharsSent();
        m_pillRenderer->applyPillHighlighting();
    }

    m_isHighlighting = false;
}

void TransmitTextEdit::beginInternalDocumentMutation() {
    m_sourceMirror->beginInternalMutation();
}

void TransmitTextEdit::endInternalDocumentMutation() {
    m_sourceMirror->endInternalMutation();
}

bool TransmitTextEdit::isInternalDocumentMutationActive() const {
    return m_sourceMirror->isInternalMutationActive();
}

QTextCursor::MoveOperation deleteKeyEventToMoveOperation(QKeyEvent *e) {
    QTextCursor::MoveOperation op = QTextCursor::NoMove;

    if (e == QKeySequence::DeleteStartOfWord) {
        op = QTextCursor::StartOfWord;
    } else if (e == QKeySequence::DeleteEndOfWord) {
        op = QTextCursor::EndOfWord;
    } else if (e == QKeySequence::DeleteCompleteLine) {
        op = QTextCursor::StartOfLine;
    } else if (e == QKeySequence::DeleteEndOfLine) {
        op = QTextCursor::EndOfLine;
    }

    return op;
}

bool isDeleteKeyEvent(QKeyEvent *e) {
    return deleteKeyEventToMoveOperation(e) != QTextCursor::NoMove;
}

QTextCursor::MoveOperation movementKeyEventToMoveOperation(QKeyEvent *e) {
    QTextCursor::MoveOperation op = QTextCursor::NoMove;

    if (e == QKeySequence::Delete) {
        op = QTextCursor::PreviousCharacter;
    } else if (e == QKeySequence::DeleteStartOfWord) {
        op = QTextCursor::StartOfWord;
    } else if (e == QKeySequence::DeleteEndOfWord) {
        op = QTextCursor::EndOfWord;
    } else if (e == QKeySequence::DeleteCompleteLine) {
        op = QTextCursor::StartOfLine;
    } else if (e == QKeySequence::DeleteEndOfLine) {
        op = QTextCursor::EndOfLine;
    } else if (e == QKeySequence::MoveToNextChar) {
        op = QTextCursor::Right;
    } else if (e == QKeySequence::MoveToPreviousChar) {
        op = QTextCursor::Left;
    } else if (e == QKeySequence::SelectNextChar) {
        op = QTextCursor::Right;
    } else if (e == QKeySequence::SelectPreviousChar) {
        op = QTextCursor::Left;
    } else if (e == QKeySequence::SelectNextWord) {
        op = QTextCursor::WordRight;
    } else if (e == QKeySequence::SelectPreviousWord) {
        op = QTextCursor::WordLeft;
    } else if (e == QKeySequence::SelectStartOfLine) {
        op = QTextCursor::StartOfLine;
    } else if (e == QKeySequence::SelectEndOfLine) {
        op = QTextCursor::EndOfLine;
    } else if (e == QKeySequence::SelectStartOfBlock) {
        op = QTextCursor::StartOfBlock;
    } else if (e == QKeySequence::SelectEndOfBlock) {
        op = QTextCursor::EndOfBlock;
    } else if (e == QKeySequence::SelectStartOfDocument) {
        op = QTextCursor::Start;
    } else if (e == QKeySequence::SelectEndOfDocument) {
        op = QTextCursor::End;
    } else if (e == QKeySequence::SelectPreviousLine) {
        op = QTextCursor::Up;
    } else if (e == QKeySequence::SelectNextLine) {
        op = QTextCursor::Down;
    } else if (e == QKeySequence::MoveToNextWord) {
        op = QTextCursor::WordRight;
    } else if (e == QKeySequence::MoveToPreviousWord) {
        op = QTextCursor::WordLeft;
    } else if (e == QKeySequence::MoveToEndOfBlock) {
        op = QTextCursor::EndOfBlock;
    } else if (e == QKeySequence::MoveToStartOfBlock) {
        op = QTextCursor::StartOfBlock;
    } else if (e == QKeySequence::MoveToNextLine) {
        op = QTextCursor::Down;
    } else if (e == QKeySequence::MoveToPreviousLine) {
        op = QTextCursor::Up;
    } else if (e == QKeySequence::MoveToPreviousLine) {
        op = QTextCursor::Up;
    } else if (e == QKeySequence::MoveToStartOfLine) {
        op = QTextCursor::StartOfLine;
    } else if (e == QKeySequence::MoveToEndOfLine) {
        op = QTextCursor::EndOfLine;
    } else if (e == QKeySequence::MoveToStartOfDocument) {
        op = QTextCursor::Start;
    } else if (e == QKeySequence::MoveToEndOfDocument) {
        op = QTextCursor::End;
    }

    return op;
}

bool isMovementKeyEvent(QKeyEvent *k) {
    return movementKeyEventToMoveOperation(k) != QTextCursor::NoMove;
}

bool TransmitTextEdit::eventFilter(QObject * /*o*/, QEvent *e) {
    if (e->type() != QEvent::KeyPress) {
        return false;
    }

    // -1. don't filter the escape key, return key, or enter key here
    QKeyEvent *k = static_cast<QKeyEvent *>(e);
    if (k->key() == Qt::Key_Escape) {
        return false;
    }

    if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
        return false;
    }

    if (k == QKeySequence::Undo || k == QKeySequence::Redo) {
        return false;
    }

    auto c = textCursor();

    auto c2 = QTextCursor(c);

    auto op = movementKeyEventToMoveOperation(k);
    c2.movePosition(op);

    bool shouldBeProtected = cursorShouldBeProtected(c2);

    // -1. if this is a delete event (like Ctrl+Backspace) and it should be
    // protected, force protection
    if (isDeleteKeyEvent(k)) {
        // if we technically shouldn't be protected...check to see if the
        // previous character is a space if it is, let's try the move operation
        // from before the space, since the space can cause word coallescing in
        // the qtextdocument making the operation not actually move the cursor
        if (!shouldBeProtected) {
            auto c3 = QTextCursor(c);
            c3.movePosition(QTextCursor::PreviousCharacter);
            if (c.document()
                    ->characterAt(qMin(c3.selectionStart(), c3.selectionEnd()))
                    .isSpace()) {
                c3.movePosition(op);
                shouldBeProtected = cursorShouldBeProtected(c3);
            }
        }

        if (shouldBeProtected) {
            k->ignore();
            return true;
        }
    }

    // 0. only filter when in a protected range
    // 0b. but only if we're not moving/deleting _into_ the protected range :/
    if (!isProtected() && !shouldBeProtected) {
        return false;
    }

    // 1. do not filter movement sequences
    if (isMovementKeyEvent(k)) {
        return false;
    }

    // 2. if on the edge, do not filter if not a backspace
    int start = qMin(c.selectionStart(), c.selectionEnd());
    if (start == m_sent && k->key() != Qt::Key_Backspace) {
        return false;
    }

    // 3. if on the edge, do not filter if a backspace and there is text
    // selected
    int end = qMax(c.selectionStart(), c.selectionEnd());
    if (start == m_sent && start != end && k->key() == Qt::Key_Backspace) {
        return false;
    }

    return true;
}

void TransmitTextEdit::keyPressEvent(QKeyEvent *e) {
    if (e == QKeySequence::Undo) {
        undo();
        e->accept();
        return;
    }

    if (e == QKeySequence::Redo) {
        redo();
        e->accept();
        return;
    }

    QTextEdit::keyPressEvent(e);
}

Q_LOGGING_CATEGORY(transmittextedit_js8, "transmittextedit.js8", QtWarningMsg)
