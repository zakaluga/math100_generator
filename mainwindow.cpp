#include "mainwindow.h"
#include "webenginehost.h"
#include "networkmanager.h"
#include "exportdialog.h"
#include <QToolBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QDesktopServices>
#include <QUrl>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_urlEdit(new QLineEdit(this))
    , m_fetchButton(new QPushButton("Загрузить вариант", this))
    , m_clearButton(new QPushButton("Очистить кэш", this))
    , m_origPdfButton(new QPushButton("Оригинальный PDF сайта", this))
    , m_exportButton(new QPushButton("Экспорт PDF…", this))
    , m_studentButton(new QPushButton("PDF: все задания (студент)", this))
    , m_teacherButton(new QPushButton("PDF: задания+ответы+решения (преподаватель)", this))
    , m_progressBar(new QProgressBar(this))
    , m_taskList(new QListWidget(this))
    , m_statusLabel(new QLabel("Готов", this))
    , m_viewContainer(nullptr)
    , m_webEngine(nullptr)
    , m_networkManager(nullptr)
    , m_currentTaskIndex(0)
{
    // Сначала создаём компоненты, потом UI:
    // setupUI() использует m_webEngine->view() (m_viewContainer)
    m_networkManager = new NetworkManager(this);
    m_webEngine = new WebEngineHost(this, m_networkManager);

    setupUI();

    connect(m_fetchButton, &QPushButton::clicked, this, &MainWindow::onFetchUrl);
    connect(m_taskList, &QListWidget::currentRowChanged, this, &MainWindow::onTaskSelected);
    connect(m_clearButton, &QPushButton::clicked, this, &MainWindow::onClearCache);
    connect(m_origPdfButton, &QPushButton::clicked, this, &MainWindow::onOriginalPdf);
    // task-22: результат загрузки вариант-PDF.
    connect(m_networkManager, &NetworkManager::variantPdfFinished, this,
            [this](bool ok, const QString &message) {
        m_origPdfButton->setEnabled(true);
        if (ok) {
            updateStatus("Оригинальный PDF сохранён: " + message);
        } else {
            updateStatus("Ошибка загрузки PDF: " + message);
            QMessageBox::critical(this, "Ошибка", "Не удалось скачать оригинальный PDF:\n" + message);
        }
    });
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::onExportDialog);
    connect(m_studentButton, &QPushButton::clicked, this, &MainWindow::onPresetStudent);
    connect(m_teacherButton, &QPushButton::clicked, this, &MainWindow::onPresetTeacher);
    connect(m_webEngine, &WebEngineHost::statusChanged, this, &MainWindow::updateStatus);
    connect(m_networkManager, &NetworkManager::downloadProgress, this, [this](int percent) {
        if (percent >= 0) {
            m_progressBar->setValue(percent);
        }
    });
    connect(m_networkManager, &NetworkManager::pageListFetched, this, [this](const QStringList &urls) {
        m_taskUrls = urls;
        m_taskList->clear();
        // task-10: кап 30 убран — список показывает ВСЕ задачи варианта
        // (проф-варианты: 33, базовые: 18).
        for (int i = 0; i < urls.size(); ++i) {
            m_taskList->addItem(QString("Задача %1").arg(i + 1));
        }
        m_taskList->setCurrentRow(0);
        m_progressBar->setValue(100);

        // task-22: второй источник — оригинальный PDF сайта (data-m100-pdf-src).
        const bool hasSitePdf = !m_networkManager->variantPdfUrl().isEmpty();
        m_origPdfButton->setVisible(hasSitePdf);
        if (urls.isEmpty() && hasSitePdf) {
            // «canvas-only» страница (демо-варианты): HTML-задач нет,
            // единственный материал — PDF сайта.
            updateStatus("HTML-задачи не найдены — доступен только "
                         "«Оригинальный PDF сайта»");
        } else if (hasSitePdf) {
            updateStatus(QString("Загружено %1 задач. Доступен "
                                 "«Оригинальный PDF сайта»")
                         .arg(urls.size()));
        } else {
            updateStatus(QString("Загружено %1 задач").arg(urls.size()));
        }
        disableUI(false);
    });
    connect(m_networkManager, &NetworkManager::errorOccurred, this, [this](const QString &error) {
        QMessageBox::critical(this, "Ошибка", error);
        disableUI(false);
        m_progressBar->setValue(0);
    });

    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    disableUI(false);

    setWindowTitle("Math100 PDF Generator v1.0.0");
    resize(1200, 800);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUI()
{
    auto *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setSpacing(8);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    // Top bar: URL input + buttons
    auto *topLayout = new QHBoxLayout();
    m_urlEdit->setPlaceholderText("Введите URL варианта, например: https://math100.ru/prof-ege-2027-10-1/");
    m_urlEdit->setMinimumHeight(35);
    topLayout->addWidget(m_urlEdit);
    topLayout->addWidget(m_fetchButton);
    mainLayout->addLayout(topLayout);

    // Progress bar
    mainLayout->addWidget(m_progressBar);

    // Middle: task list + web view
    auto *middleLayout = new QHBoxLayout();

    // Left: task list
    auto *listLayout = new QVBoxLayout();
    listLayout->addWidget(new QLabel("Список задач:"));
    listLayout->addWidget(m_taskList);
    middleLayout->addLayout(listLayout, 1);

    // Right: web view
    m_viewContainer = new QWidget(this);
    auto *viewLayout = new QVBoxLayout(m_viewContainer);
    viewLayout->setContentsMargins(0, 0, 0, 0);
    viewLayout->addWidget(m_webEngine->view());
    middleLayout->addWidget(m_viewContainer, 3);

    mainLayout->addLayout(middleLayout, 10);

    // Bottom bar 1: [Оригинальный PDF сайта — по наличию] + clear + status
    // (task-21: кнопка «Скачать PDF» удалена — рудимент, функцию полностью
    //  покрывает «Экспорт PDF…» с выбором одной задачи; task-22: m_origPdfButton
    //  видна только если у варианта есть data-m100-pdf-src)
    auto *bottomLayout = new QHBoxLayout();
    m_origPdfButton->hide();
    bottomLayout->addWidget(m_origPdfButton);
    bottomLayout->addWidget(m_clearButton);
    bottomLayout->addStretch();
    bottomLayout->addWidget(m_statusLabel);
    mainLayout->addLayout(bottomLayout);

    // Bottom bar 2: export (диалог выбора + пресеты студент/преподаватель)
    auto *exportLayout = new QHBoxLayout();
    auto *exportLabel = new QLabel("Экспорт:", this);
    exportLabel->setStyleSheet("QLabel { font-weight: bold; color: #444444; }");
    exportLayout->addWidget(exportLabel);
    exportLayout->addWidget(m_exportButton);
    exportLayout->addWidget(m_studentButton);
    exportLayout->addWidget(m_teacherButton);
    exportLayout->addStretch();
    mainLayout->addLayout(exportLayout);

    // Styles
    m_fetchButton->setStyleSheet(
        "QPushButton { padding: 6px 16px; font-weight: bold; background-color: #4CAF50; color: white; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #45a049; }"
        "QPushButton:disabled { background-color: #cccccc; }"
    );
    m_clearButton->setStyleSheet(
        "QPushButton { padding: 6px 16px; background-color: #f44336; color: white; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #d32f2f; }"
    );
    // task-22: синий — «скачать готовый файл сайта» (не экспорт).
    m_origPdfButton->setStyleSheet(
        "QPushButton { padding: 6px 16px; font-weight: bold; background-color: #2196F3; color: white; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #1976D2; }"
        "QPushButton:disabled { background-color: #cccccc; }"
    );
    m_exportButton->setStyleSheet(
        "QPushButton { padding: 6px 14px; font-weight: bold; background-color: #607D8B; color: white; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #546E7A; }"
        "QPushButton:disabled { background-color: #cccccc; }"
    );
    m_studentButton->setStyleSheet(
        "QPushButton { padding: 6px 14px; font-weight: bold; background-color: #2196F3; color: white; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #1976D2; }"
        "QPushButton:disabled { background-color: #cccccc; }"
    );
    m_teacherButton->setStyleSheet(
        "QPushButton { padding: 6px 14px; font-weight: bold; background-color: #673AB7; color: white; border: none; border-radius: 4px; }"
        "QPushButton:hover { background-color: #512DA8; }"
        "QPushButton:disabled { background-color: #cccccc; }"
    );
    m_urlEdit->setStyleSheet(
        "QLineEdit { padding: 6px; border: 1px solid #cccccc; border-radius: 4px; }"
    );
    m_taskList->setStyleSheet(
        "QListWidget { border: 1px solid #cccccc; border-radius: 4px; }"
        "QListWidget::item:selected { background-color: #e3f2fd; }"
    );
    m_statusLabel->setStyleSheet("QLabel { color: #666666; }");
}

void MainWindow::onFetchUrl()
{
    QString url = m_urlEdit->text().trimmed();
    if (url.isEmpty()) {
        QMessageBox::warning(this, "Внимание", "Введите URL варианта");
        return;
    }

    disableUI(true);
    m_progressBar->setValue(0);
    m_taskList->clear();
    // task-22: пока вариант не загружен — старый PDF-URL не актуален.
    m_origPdfButton->hide();
    updateStatus("Загрузка страницы варианта...");

    m_networkManager->fetchVariantPage(url);
}

void MainWindow::onTaskSelected(int index)
{
    if (index < 0 || index >= m_taskUrls.size()) {
        return;
    }
    m_currentTaskIndex = index;
    updateStatus(QString("Загрузка задачи %1...").arg(index + 1));

    // Подключаемся к сигналу один раз
    static bool connected = false;
    if (!connected) {
        connect(m_networkManager, &NetworkManager::taskPageFetched,
                this, &MainWindow::onTaskPageLoaded);
        connected = true;
    }

    m_networkManager->fetchTaskPage(m_taskUrls[index]);
}

void MainWindow::onTaskPageLoaded(const QString &html, const QString &url)
{
    m_webEngine->loadTaskPage(html, url);
    updateStatus(QString("Задача %1 загружена").arg(m_currentTaskIndex + 1));
}

void MainWindow::onClearCache()
{
    if (m_networkManager) {
        m_networkManager->clearCache();
        QMessageBox::information(this, "Кэш очищен", "Кэш страниц и изображений очищен");
    }
}

void MainWindow::onOriginalPdf()
{
    // task-22: скачать оригинальный PDF варианта со сайта (pdf.math100.ru).
    if (!m_networkManager || m_networkManager->variantPdfUrl().isEmpty()) {
        return;
    }

    // Имя по умолчанию: последний сегмент URL варианта + ".pdf".
    QString base = "variant";
    const QStringList segments = QUrl(m_urlEdit->text().trimmed()).path()
        .split('/', Qt::SkipEmptyParts);
    if (!segments.isEmpty()) {
        const QString last = segments.last().simplified();
        if (!last.isEmpty()) {
            base = last;
        }
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    QDir().mkpath(dir);

    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Сохранить оригинальный PDF варианта"),
        dir + '/' + base + ".pdf",
        tr("PDF-файлы (*.pdf)"));
    if (path.isEmpty()) {
        return; // отмена
    }
    if (!path.endsWith(".pdf", Qt::CaseInsensitive)) {
        path += ".pdf";
    }

    m_origPdfButton->setEnabled(false);
    updateStatus("Скачивание оригинального PDF...");
    m_networkManager->downloadVariantPdf(path);
}

void MainWindow::onExportDialog()
{
    if (m_taskUrls.isEmpty()) {
        QMessageBox::information(this, "Экспорт PDF", "Сначала загрузите вариант");
        return;
    }

    QStringList names;
    names.reserve(m_taskUrls.size());
    for (int i = 0; i < m_taskUrls.size(); ++i) {
        names << QString("Задача %1").arg(i + 1);
    }
    ExportDialog dlg(names, this);
    if (dlg.exec() != QDialog::Accepted) {
        return;
    }
    // task-10: формат берём из диалога (таблица/страницы) — 6-й аргумент buildExportDocument.
    onExport(dlg.selectedIndices(), dlg.withAnswer(), dlg.withSolution(),
             "-export", dlg.useTable());
}

void MainWindow::onPresetStudent()
{
    QList<int> indices(m_taskUrls.size());
    for (int i = 0; i < m_taskUrls.size(); ++i) {
        indices[i] = i;
    }
    // task-10: студентский пресет — ТАБЛИЦА (краткая форма): компактный список
    // всех заданий на 1-2 страницах, без ответов/решений.
    onExport(indices, false, false, "-student", /*useTable=*/true);
}

void MainWindow::onPresetTeacher()
{
    QList<int> indices(m_taskUrls.size());
    for (int i = 0; i < m_taskUrls.size(); ++i) {
        indices[i] = i;
    }
    // task-10: преподавательский пресет — СТРАНИЦЫ (полная форма): задания с
    // ответами и решениями читаются удобнее на отдельных страницах, где
    // inline-формулы раскрываются в display (на отдельных строках); таблица
    // со решениями была бы перегружена. Осознанный выбор, не баг.
    onExport(indices, true, true, "-teacher", /*useTable=*/false);
}

void MainWindow::onExport(const QList<int> &indices, bool withAnswer, bool withSolution,
                          const QString &suffix, bool useTable)
{
    if (m_taskUrls.isEmpty()) {
        QMessageBox::information(this, "Экспорт PDF", "Сначала загрузите вариант");
        return;
    }
    if (indices.isEmpty()) {
        updateStatus("Экспорт пуст: не выбрано ни одной задачи");
        return;
    }

    disableUI(true);
    m_progressBar->setValue(0);
    updateStatus("Формирование экспорта (скачивание недостающих задач)...");

    // Блокирующий вызов: прогресс скачиваний идёт в m_progressBar через
    // существующий connect downloadProgress.
    // useTable: false — страницы (полная форма), true — таблица (краткая, компактно).
    QString doc = m_networkManager->buildExportDocument(m_taskUrls, indices, true, withAnswer, withSolution, useTable);

    if (doc.isEmpty()) {
        updateStatus("Экспорт пуст");
        disableUI(false);
        m_progressBar->setValue(0);
        return;
    }

    // Имя по умолчанию: последний сегмент пути URL варианта + suffix + ".pdf"
    // (напр. prof-ege-2027-10-1-student.pdf), каталог — Download (fallback home).
    QString base = "variant";
    const QStringList segments = QUrl(m_urlEdit->text().trimmed()).path().split('/', Qt::SkipEmptyParts);
    if (!segments.isEmpty()) {
        const QString last = segments.last().simplified();
        if (!last.isEmpty()) {
            base = last;
        }
    }
    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    QDir().mkpath(dir);

    QString path = QFileDialog::getSaveFileName(
        this,
        tr("Сохранить PDF экспорта"),
        dir + '/' + base + suffix + ".pdf",
        tr("PDF-файлы (*.pdf)"));
    if (path.isEmpty()) {
        updateStatus("Экспорт отменён");
        disableUI(false);
        m_progressBar->setValue(0);
        return;
    }
    if (!path.endsWith(".pdf", Qt::CaseInsensitive)) {
        path += ".pdf";
    }

    const bool ok = m_webEngine->printHtmlToPdf(doc, path, 60);
    updateStatus(ok ? tr("PDF сохранён: %1").arg(path) : tr("Ошибка создания PDF"));
    disableUI(false);
    m_progressBar->setValue(ok ? 100 : 0);
}

void MainWindow::updateStatus(const QString &msg)
{
    m_statusLabel->setText(QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"))
                           .arg(msg));
}

void MainWindow::disableUI(bool disabled)
{
    m_fetchButton->setEnabled(!disabled);
    m_urlEdit->setEnabled(!disabled);
    m_taskList->setEnabled(!disabled);
    m_origPdfButton->setEnabled(!disabled);
    m_exportButton->setEnabled(!disabled);
    m_studentButton->setEnabled(!disabled);
    m_teacherButton->setEnabled(!disabled);
}
