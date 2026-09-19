#include "exportdialog.h"
#include <QListWidget>
#include <algorithm>
#include <QCheckBox>
#include <QRadioButton>
#include <QButtonGroup>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QHBoxLayout>

ExportDialog::ExportDialog(const QStringList &taskNames, QWidget *parent)
    : QDialog(parent)
    , m_list(new QListWidget(this))
    , m_answerCheck(new QCheckBox(tr("Ответы"), this))
    , m_solutionCheck(new QCheckBox(tr("Решения"), this))
    , m_formatGroup(new QButtonGroup(this))
    , m_pagesRadio(new QRadioButton(tr("По страницам (полная форма)"), this))
    , m_tableRadio(new QRadioButton(tr("В таблицу (краткая форма, компактно)"), this))
{
    setWindowTitle(tr("Экспорт PDF"));
    setMinimumWidth(420);

    // Список задач: чекбоксы «Задача N»
    m_list->setStyleSheet(
        "QListWidget { border: 1px solid #cccccc; border-radius: 4px; }"
        "QListWidget::item { padding: 4px; }"
        "QListWidget::item:selected { background-color: #e3f2fd; }");
    for (int i = 0; i < taskNames.size(); ++i) {
        auto *item = new QListWidgetItem(taskNames.value(i));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked); // по умолчанию — все выбраны
        m_list->addItem(item);
    }

    // Кнопки «Выбрать все» / «Снять выбор»
    auto *selectBtn = new QPushButton(tr("Выбрать все"), this);
    auto *deselectBtn = new QPushButton(tr("Снять выбор"), this);
    for (QPushButton *b : {selectBtn, deselectBtn}) {
        b->setStyleSheet(
            "QPushButton { padding: 5px 14px; border: 1px solid #b0bec5; border-radius: 4px; "
            "background-color: #eceff1; }"
            "QPushButton:hover { background-color: #cfd8dc; }");
    }
    auto *selectRow = new QHBoxLayout();
    selectRow->addWidget(selectBtn);
    selectRow->addWidget(deselectBtn);
    selectRow->addStretch();

    // Контент
    auto *taskCheck = new QCheckBox(tr("Задания"), this);
    taskCheck->setChecked(true);
    taskCheck->setEnabled(false); // всегда включено
    m_answerCheck->setChecked(false);
    m_solutionCheck->setChecked(false);

    auto *contentRow = new QHBoxLayout();
    contentRow->addWidget(taskCheck);
    contentRow->addWidget(m_answerCheck);
    contentRow->addWidget(m_solutionCheck);
    contentRow->addStretch();

    // task-10: Формат документа (эксклюзивная группа radio).
    // Дефолт — ТАБЛИЦА (краткая, компактно): пользователь хочет компактнее.
    m_formatGroup->addButton(m_pagesRadio, 0);
    m_formatGroup->addButton(m_tableRadio, 1);
    m_formatGroup->setExclusive(true);
    m_tableRadio->setChecked(true);

    auto *formatRow = new QHBoxLayout();
    formatRow->addWidget(m_pagesRadio);
    formatRow->addWidget(m_tableRadio);
    formatRow->addStretch();

    // OK / Cancel
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(selectBtn, &QPushButton::clicked, this, &ExportDialog::selectAll);
    connect(deselectBtn, &QPushButton::clicked, this, &ExportDialog::deselectAll);

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(new QLabel(tr("Задачи (отметьте нужное):"), this));
    mainLayout->addWidget(m_list, 1);
    mainLayout->addLayout(selectRow);
    mainLayout->addSpacing(6);
    mainLayout->addWidget(new QLabel(tr("Контент:"), this));
    mainLayout->addLayout(contentRow);
    mainLayout->addSpacing(6);
    mainLayout->addWidget(new QLabel(tr("Формат:"), this));
    mainLayout->addLayout(formatRow);
    mainLayout->addWidget(buttons);

    setAllChecked(true);
}

QList<int> ExportDialog::selectedIndices() const
{
    QList<int> indices;
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->checkState() == Qt::Checked) {
            indices.append(i);
        }
    }
    std::sort(indices.begin(), indices.end());
    return indices;
}

bool ExportDialog::withAnswer() const
{
    return m_answerCheck->isChecked();
}

bool ExportDialog::withSolution() const
{
    return m_solutionCheck->isChecked();
}

bool ExportDialog::useTable() const
{
    // true — таблица (краткая форма), false — страницы (полная форма).
    return m_tableRadio->isChecked();
}

void ExportDialog::selectAll()
{
    setAllChecked(true);
}

void ExportDialog::deselectAll()
{
    setAllChecked(false);
}

void ExportDialog::setAllChecked(bool checked)
{
    for (int i = 0; i < m_list->count(); ++i) {
        m_list->item(i)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}
