#include "sauce_dialog.h"

#include "blockwright/cp437.h"

#include <QDate>
#include <QDialogButtonBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QValidator>

namespace bw {

namespace {

// Refuses any edit that would put a character CP437 can't store in the field.
class Cp437Validator : public QValidator {
public:
    using QValidator::QValidator;
    State validate(QString& input, int&) const override { return toCp437(input) ? Acceptable : Invalid; }
};

QString trimEnd(QString s) {
    while (s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    return s;
}

std::string bytes(const QString& s) { return toCp437(trimEnd(s)).value_or(std::string()); }

QLineEdit* textField(const std::string& value, size_t limit, QWidget* parent) {
    auto* e = new QLineEdit(parent);
    e->setMaxLength(int(limit));
    e->setValidator(new Cp437Validator(e));
    e->setText(fromCp437(value));
    return e;
}

// The comment text as typed. toPlainText() would make U+00A0 (CP437 0xFF)
// a space; the raw text keeps it and has U+2029 or U+2028 for line breaks.
QString commentText(const QPlainTextEdit* e) {
    QString text = e->document()->toRawText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    text.replace(QChar::LineSeparator, QLatin1Char('\n'));
    return text;
}

QStringList commentLines(const QString& text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& l : lines)
        l = trimEnd(l);
    while (!lines.isEmpty() && lines.back().isEmpty())
        lines.removeLast();
    return lines;
}

} // namespace

std::optional<std::string> toCp437(const QString& text) {
    std::string out;
    for (const char32_t ch : text.normalized(QString::NormalizationForm_C).toUcs4()) {
        const std::optional<std::uint8_t> b = unicodeToCp437(ch);
        if (!b || *b == 0)
            return std::nullopt;
        out += static_cast<char>(*b);
    }
    return out;
}

QString fromCp437(const std::string& bytes) { return QString::fromStdString(cp437ToUtf8(bytes)); }

SauceDialog::SauceDialog(const Sauce& sauce, QWidget* parent) : QDialog(parent), shown_(sauce) {
    setWindowTitle(tr("SAUCE"));
    title_ = textField(sauce.title, Sauce::kTitleLen, this);
    title_->setObjectName(QStringLiteral("sauceTitle"));
    author_ = textField(sauce.author, Sauce::kAuthorLen, this);
    author_->setObjectName(QStringLiteral("sauceAuthor"));
    group_ = textField(sauce.group, Sauce::kGroupLen, this);
    group_->setObjectName(QStringLiteral("sauceGroup"));

    date_ = new QLineEdit(QString::fromStdString(sauce.date), this);
    date_->setObjectName(QStringLiteral("sauceDate"));
    date_->setMaxLength(8);
    date_->setPlaceholderText(tr("CCYYMMDD"));
    date_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{0,8}")), date_));
    auto* today = new QPushButton(tr("&Today"), this);
    connect(today, &QPushButton::clicked, this,
            [this] { date_->setText(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))); });
    auto* dateRow = new QHBoxLayout;
    dateRow->addWidget(date_);
    dateRow->addWidget(today);

    comments_ = new QPlainTextEdit(this);
    comments_->setObjectName(QStringLiteral("sauceComments"));
    comments_->setLineWrapMode(QPlainTextEdit::NoWrap);
    QStringList lines;
    for (const std::string& c : sauce.comments)
        lines << fromCp437(c);
    comments_->setPlainText(lines.join(QLatin1Char('\n')));
    shownComments_ = commentText(comments_);
    QFont mono(QStringLiteral("monospace"));
    mono.setStyleHint(QFont::Monospace);
    comments_->setFont(mono);
    comments_->setMinimumWidth(QFontMetrics(mono).horizontalAdvance(QString(int(Sauce::kCommentLen) + 2, 'M')));

    auto* form = new QFormLayout;
    form->addRow(tr("Ti&tle (35):"), title_);
    form->addRow(tr("&Author (20):"), author_);
    form->addRow(tr("&Group (20):"), group_);
    form->addRow(tr("&Date:"), dateRow);
    form->addRow(tr("&Comments (64 per line):"), comments_);

    problem_ = new QLabel(this);
    problem_->setObjectName(QStringLiteral("sauceProblem"));
    problem_->setWordWrap(true);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(problem_);
    layout->addWidget(buttons_);

    for (QLineEdit* e : {title_, author_, group_, date_})
        connect(e, &QLineEdit::textChanged, this, &SauceDialog::check);
    connect(comments_, &QPlainTextEdit::textChanged, this, &SauceDialog::check);
    check();
}

QString SauceDialog::problem() const {
    // The line edits refuse what doesn't fit as it is typed or pasted;
    // checked again here for text set from code.
    const struct {
        QLineEdit* edit;
        const std::string& shown;
        QString name;
        size_t limit;
    } fields[] = {{title_, shown_.title, tr("The title"), Sauce::kTitleLen},
                  {author_, shown_.author, tr("The author"), Sauce::kAuthorLen},
                  {group_, shown_.group, tr("The group"), Sauce::kGroupLen}};
    for (const auto& f : fields) {
        if (f.edit->text() == fromCp437(f.shown))
            continue;
        const std::optional<std::string> b = toCp437(f.edit->text());
        if (!b)
            return tr("%1 has a character CP437 can't store.").arg(f.name);
        if (bytes(f.edit->text()).size() > f.limit)
            return tr("%1 is longer than %2 characters.").arg(f.name).arg(f.limit);
    }
    const QString date = date_->text();
    if (!date.isEmpty() && date != QString::fromStdString(shown_.date) &&
        (date.size() != 8 || !QDate::fromString(date, QStringLiteral("yyyyMMdd")).isValid()))
        return tr("The date must be empty or a date written CCYYMMDD.");
    const QString text = commentText(comments_);
    if (text == shownComments_)
        return {};
    const QStringList lines = commentLines(text);
    if (size_t(lines.size()) > Sauce::kMaxComments)
        return tr("There are %1 comment lines; SAUCE holds %2.").arg(lines.size()).arg(Sauce::kMaxComments);
    for (int i = 0; i < lines.size(); ++i) {
        const std::optional<std::string> b = toCp437(lines[i]);
        if (!b)
            return tr("Comment line %1 has a character CP437 can't store.").arg(i + 1);
        if (b->size() > Sauce::kCommentLen)
            return tr("Comment line %1 is %2 characters; the limit is %3.")
                .arg(i + 1)
                .arg(b->size())
                .arg(Sauce::kCommentLen);
    }
    return {};
}

Sauce SauceDialog::fields() const {
    const auto text = [](QLineEdit* e, const std::string& shown) {
        return e->text() == fromCp437(shown) ? shown : bytes(e->text());
    };
    Sauce s;
    s.title = text(title_, shown_.title);
    s.author = text(author_, shown_.author);
    s.group = text(group_, shown_.group);
    s.date = date_->text() == QString::fromStdString(shown_.date) ? shown_.date : date_->text().toStdString();
    const QString comments = commentText(comments_);
    if (comments == shownComments_)
        s.comments = shown_.comments;
    else
        for (const QString& l : commentLines(comments))
            s.comments.push_back(bytes(l));
    return s;
}

void SauceDialog::check() {
    const QString p = problem();
    problem_->setText(p);
    problem_->setVisible(!p.isEmpty());
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(p.isEmpty());
}

} // namespace bw
