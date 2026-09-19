#ifndef NETWORKMANAGER_H
#define NETWORKMANAGER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QMutex>
#include <QMap>
#include <QHash>

// task-9: test-харнес (offline-проверка extractTaskUrls)
class ExportFixTest;

class NetworkManager : public QObject {
    Q_OBJECT

    friend class ExportFixTest;

public:
    explicit NetworkManager(QObject *parent = nullptr);
    ~NetworkManager() override = default;

    void fetchVariantPage(const QString &variantUrl);
    void fetchTaskPage(const QString &taskUrl);
    void clearCache();

    // task-22: второй источник данных — оригинальный PDF сайта.
    // Страницы math100.ru встраивают готовый PDF (pdf.js «canvas» — это он):
    // <div class="m100-pdf" data-m100-pdf-src="https://pdf.math100.ru/pdf/<uuid>/wm.pdf">.
    // На «canvas-only» страницах (например, демо-варианты) HTML-задач НЕТ
    // вообще — PDF единственный доступный материал.
    // URL обнаруживается при fetchVariantPage (data-m100-pdf-src, fallback:
    // первая ссылка https://pdf.math100.ru/pdf/.../*.pdf). Пустая строка —
    // на странице варианта PDF нет.
    QString variantPdfUrl() const { return m_variantPdfUrl; }
    // Скачивает вариант-PDF в filePath (асинхронно; прогресс — через
    // downloadProgress; результат — через variantPdfFinished).
    void downloadVariantPdf(const QString &filePath);

    // Извлекает ТОЛЬКО контент задачи (div.post-content, balanced) из HTML страницы
    // math100.ru: убирает script/iframe/noscript, RTB-блоки, конвертирует спойлеры
    // math100-spoiler в видимые блоки task-answer-*. Чистая функция, без сети/состояния.
    static QString extractTaskContent(const QString &html, QString *titleOut = nullptr);

    // Сборка документа экспорта (самостоятельный HTML с MathJax/CSS).
    // allTaskUrls — полный список URL варианта; selectedIndices — индексы выбранных
    // задач (номер N задачи в документе = хвост URL «…-N», если последний
    // сегмент пути оканчивается на число; иначе индекс+1); флаги — какой
    // контент включать: само задание / блок «Ответ» / блок «Решение».
    // useTable: false — ПОЛНАЯ форма, одна задача на страницу (секции .pdf-task,
    //   inline-формулы \(…\) превращаются в display \[…\] — на отдельной строке, как в превью);
    // true  — КРАТКАЯ форма, все задачи в HTML-таблице (компактно; inline-формулы
    //   в предложениях остаются inline, «одиночные» формулы (текстовый блок без
    //   текста вокруг) — display: fitWideMath масштабирует их в блок-обёртке —
    //   task-11b, см. convertSoleMathSpansToDisplay).
    // Недостающие страницы докачиваются (прогресс через downloadProgress);
    // неуспешная задача → плейсхолдер, экспорт не прерывается.
    // Пустой selectedIndices -> пустая строка.
    // (ИНТЕРФЕЙС ЗАФИКСИРОВАН МЕНЕДЖЕРОМ — сигнатуру не менять; реализация: worker-a task-7/task-9)
    QString buildExportDocument(const QStringList &allTaskUrls,
                                const QList<int> &selectedIndices,
                                bool withTask, bool withAnswer, bool withSolution,
                                bool useTable = false);

signals:
    void pageListFetched(const QStringList &taskUrls);
    void taskPageFetched(const QString &html, const QString &url);
    void errorOccurred(const QString &error);
    void downloadProgress(int percent);
    // task-22: завершение загрузки вариант-PDF (ok, path или текст ошибки).
    void variantPdfFinished(bool ok, const QString &message);

private slots:
    void onReplyFinished(QNetworkReply *reply);

private:
    QString resolveUrl(const QString &relativeUrl, const QString &baseUrl) const;
    QStringList extractTaskUrls(const QString &html, const QString &baseUrl) const;
    QString fixImageUrls(const QString &html, const QString &taskUrl) const;
    QString cacheFilePath(const QString &url, const QString &subdir) const;
    QString urlToFilename(const QString &url) const;
    bool loadFromCache(const QString &url, const QString &subdir, QByteArray &data) const;
    void saveToCache(const QString &url, const QString &subdir, const QByteArray &data) const;
    void updateProgress(int percent);

    // --- task-7: buildExportDocument ----------------------------------------
    // Скачивает страницу задачи (тот же UA, что в прекаше), чистит её
    // (extractTaskContent + fixImageUrls) и кладёт в кэш "pages". Если страница
    // уже в кэше (память/диск) — сеть не используется. true = успех, чистый
    // HTML в cleanHtmlOut. Используется и прекашем, и экспортом.
    bool fetchAndCacheTaskPage(const QString &taskUrl, QString &cleanHtmlOut);

    // Разрезание чистого HTML задачи: снимает внешний div.post-content
    // (balanced), находит ВСЕ div.task-answer-block (balanced-скан, точный
    // токен класса) и классифицирует их по <strong>-лейблу (.task-answer-label):
    // содержит «ответ» (case-insensitive) -> answers; «решение» -> solutions;
    // прочее -> others. bodyOut — контент без всех блоков task-answer-block.
    static void splitTaskContent(const QString &cleanHtml, QString &bodyOut,
                                 QStringList &answers, QStringList &solutions,
                                 QStringList &others);

    // task-11b: TABLE-режим — «одиночные» формулы: \(…\) -> \[…\] (display)
    // внутри span.math, ТОЛЬКО если ближайший текстовый блок (<p> или <li>,
    // содержимое до первого вложенного <p>/<div>; span-обёртки типа
    // font-size разрешены) после ВЫЧИТАНИЯ текста всех math-спанов содержит
    // только пустоту/пробелы ИЛИ короткий «хвост» из <= 20 не-пробельных
    // символов (единицы измерения: «Па», «м/с», «K», «мин», «см/с», «В»).
    // Формула в предложении (текст вокруг > 20 не-пробельных знаков) остаётся
    // inline. Зачем: в ячейке таблицы ШИРОКАЯ inline-формула определяет
    // layout-ширину колонки (mjx-math: white-space: nowrap) -> таблица
    // шире страницы -> PDF-срез; display-формулу fitWideMath масштабирует в
    // фикс-высотной блок-обёртке (канонический JS, уже в документе).
    // Balanced-скан (как convertMathSpansToDisplay в .cpp), без regex на
    // весь документ; замены \( -> \[ / \) -> \] length-preserving.
    // Вызывается в TABLE-ветке buildExportDocument; PAGE-ветка использует
    // полный convertMathSpansToDisplay (дублировать нельзя).
    static QString convertSoleMathSpansToDisplay(const QString &html);

    // task-11b: вырезание «одиночных» formula-блоков из контента ячейки
    // таблицы. Блок = <p>/<li>, у которого (как у convertSoleMathSpansToDisplay)
    // остаточный текст после вычитания math-спанов <= 20 не-пробельных
    // символов и есть хотя бы один math-спан: его формулы конвертируются
    // в display (\[...\]) и САМ БЛОК вырезается из html (возврат — очищенный
    // контент), а блок (с display-формулами) — в *extracted, в порядке
    // следования. Зачем: fitWideMath масштабирует display-формулу под
    // body.clientWidth (ширину СТРАНИЦЫ); в узкой ячейке 4-колоночной
    // таблицы такая формула либо обрезается fit-обёрткой (overflow:hidden),
    // либо (без wrap) выбегает за край страницы. В ПОЛНОЙ ШИРИНЫ строке
    // (colspan, td.formula-cell без padding/border L-R) обёртка = страница —
    // формула вписывается точно (см. DOM-замер task-11b).
    static QString extractSoleMathBlocks(const QString &html,
                                         QStringList *extracted);

    // Секция одной задачи в документе экспорта (N = оригинальный номер задачи
    // в варианте). Порядок: задание -> ответ -> решение; «other»-блоки — всегда.
    static QString buildTaskSection(int number, const QString &body,
                                    const QStringList &answers,
                                    const QStringList &solutions,
                                    const QStringList &others,
                                    bool withTask, bool withAnswer,
                                    bool withSolution);
    // Секция-плейсхолдер для задачи, которую не удалось загрузить.
    static QString buildMissingSection(int number, const QString &url);
    // Полный самостоятельный HTML-документ (MathJax + CSS, page-break'и между
    // задачами) — для WebEngine printToPdf. task-9: useTable — табличный
    // режим (body 11pt + CSS .export-table); bodyHtml — содержимое <body>
    // (секции .pdf-task или таблица).
    static QString buildExportPageHtml(const QString &bodyHtml, bool useTable = false);

    // task-22: детект URL оригинального PDF в HTML страницы варианта
    // (чистая функция — вынесена для тестуемости, как extractTaskUrls).
    static QString extractVariantPdfUrl(const QString &html);

    mutable QNetworkAccessManager m_networkManager;
    QString m_variantBaseUrl;
    QString m_variantPdfUrl;   // task-22: "" — на странице варианта PDF нет
    bool m_pdfDownloadInProgress = false;
    QHash<QString, QNetworkReply*> m_activeReplies;
    mutable QMap<QString, QByteArray> m_pageCache;
    mutable QMutex m_cacheMutex;
};

#endif // NETWORKMANAGER_H
