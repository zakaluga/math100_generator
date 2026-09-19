#ifndef WEBENGINEHOST_H
#define WEBENGINEHOST_H

#include <QObject>
#include <QWidget>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebChannel>
#include <QUrl>
#include <QString>
#include <QByteArray>

class NetworkManager;

class WebEngineHost : public QObject {
    Q_OBJECT

public:
    explicit WebEngineHost(QWidget *parent = nullptr, NetworkManager *networkMgr = nullptr);
    ~WebEngineHost() override = default;

    QWebEngineView *view() const { return m_view; }
    void loadTaskPage(const QString &html, const QString &taskUrl);
    void triggerPrint();
    // Генерирует PDF текущей страницы (C++-сторона, QtWebEngine printToPdf).
    // Синхронно: ждёт готовности MathJax (таймаут ~10s), затем ПЕРЕД печатью
    // выполняет window.fitWideMath() (подгонка широких display-формул,
    // task-12) и ждёт её callback (backstop 5s); printToPdf вызывается внутри
    // этого callback (таймаут 30s). Возвращает true, если файл успешно записан.
    bool printPdfTo(const QString &filePath);
    // Генерирует PDF из готового самодостаточного HTML-документа (экспорт).
    // task-18: печатает на ОСНОВНОЙ m_page (её рендер-пайплайн живой —
    // пользователь видит превью), а НЕ на временной странице: временная
    // QWebEnginePage в отдельном view на Windows 11 / Qt 6.10.3 давала
    // ПУСТОЕ окно (кадры не композитятся) и висящий printToPdf (все
    // эскалации task-16a/17 пусты, логи 17:06 и 20:30). Механика:
    // сохраняем текущее превью (m_currentTaskHtml/Url) → setHtml экспортного
    // документа в m_page (пользователь видит то, что печатается) →
    // MathJax-ожидание (backstop mathjaxWaitSec) → fitWideMath (callback,
    // backstop 5s, task-12) → printToPdf (backstop 300с + heartbeat, task-17)
    // → ВОССТАНАВЛИВАЕМ превью → запись файла. Синхронно.
    bool printHtmlToPdf(const QString &fullHtml, const QString &filePath, int mathjaxWaitSec = 60);
    QString currentTaskUrl() const { return m_currentTaskUrl; }

public slots:
    void clearContent();

signals:
    void statusChanged(const QString &message);

private:
    void setupWebView();
    void setupWebChannel();
    void showPlaceholder(const QString &message);
    QString createFullHtml(const QString &taskHtml);

    QWebEngineView *m_view;
    QWebEnginePage *m_page;
    QWebChannel *m_channel;
    NetworkManager *m_networkManager;
    QString m_currentTaskUrl;
    // task-18: полный HTML последнего превью (createFullHtml-обёртка) —
    // чтобы вернуть его в m_page после экспортной печати.
    QString m_currentTaskHtml;
    bool m_initialized;
    bool m_pdfBusy;
};

#endif // WEBENGINEHOST_H
