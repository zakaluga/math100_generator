#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QPushButton>
#include <QListWidget>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QLabel>
#include <QList>

class WebEngineHost;
class NetworkManager;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // task-19: доступ к хосту для print-selftest (MATH100_SELFTEST_PRINT)
    // в main.cpp.
    WebEngineHost *webEngine() const { return m_webEngine; }

private slots:
    void onFetchUrl();
    void onTaskSelected(int index);
    void onTaskPageLoaded(const QString &html, const QString &url);
    void onClearCache();
    void onExportDialog();
    void onPresetStudent();
    void onPresetTeacher();

private:
    void setupUI();
    void updateStatus(const QString &msg);
    void disableUI(bool disabled);
    // Общий экспорт: buildExportDocument → диалог сохранения → printHtmlToPdf.
    // useTable: false — страницы (полная форма), true — таблица (краткая, компактно).
    void onExport(const QList<int> &indices, bool withAnswer, bool withSolution,
                  const QString &suffix, bool useTable);

    // UI controls
    QLineEdit *m_urlEdit;
    QPushButton *m_fetchButton;
    QPushButton *m_clearButton;
    QPushButton *m_exportButton;
    QPushButton *m_studentButton;
    QPushButton *m_teacherButton;
    QProgressBar *m_progressBar;
    QListWidget *m_taskList;
    QLabel *m_statusLabel;
    QWidget *m_viewContainer;

    // Core components
    WebEngineHost *m_webEngine;
    NetworkManager *m_networkManager;

    // State
    QStringList m_taskUrls;
    int m_currentTaskIndex;
};

#endif // MAINWINDOW_H
