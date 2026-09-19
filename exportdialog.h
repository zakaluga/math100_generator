#ifndef EXPORTDIALOG_H
#define EXPORTDIALOG_H

#include <QDialog>
#include <QList>

class QListWidget;
class QCheckBox;
class QRadioButton;
class QButtonGroup;

// Диалог экспорта PDF: выбор задач (чекбоксы), контента (задание/ответ/решение)
// и формата (страницы/таблица).
// «Задания» всегда включены (экспорт без них бессмыслен) — метод withTask() не нужен.
class ExportDialog : public QDialog {
    Q_OBJECT

public:
    // taskNames — имена задач для списка (обычно «Задача N», N = i+1).
    explicit ExportDialog(const QStringList &taskNames, QWidget *parent = nullptr);

    // Индексы отмеченных задач (0-based, совпадают с m_taskUrls), по возрастанию.
    QList<int> selectedIndices() const;
    bool withAnswer() const;
    bool withSolution() const;
    // task-10: формат документа экспорта. true — ТАБЛИЦА (краткая форма,
    // компактно, все задачи в одной HTML-таблице); false — СТРАНИЦЫ (полная
    // форма, задача на страницу, формулы на отдельных строках).
    // По умолчанию — ТАБЛИЦА (пользователь хочет компактнее).
    bool useTable() const;

private slots:
    void selectAll();
    void deselectAll();

private:
    void setAllChecked(bool checked);

    QListWidget *m_list;
    QCheckBox *m_answerCheck;
    QCheckBox *m_solutionCheck;
    QButtonGroup *m_formatGroup;   // task-10: эксклюзивная группа форматов
    QRadioButton *m_pagesRadio;    // false: страницы (полная форма)
    QRadioButton *m_tableRadio;    // true:  таблица (краткая форма)
};

#endif // EXPORTDIALOG_H
