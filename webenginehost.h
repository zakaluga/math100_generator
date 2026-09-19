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
    // Генерирует PDF из готового самодостаточного HTML-документа (экспорт)
    // на отдельной временной QWebEnginePage (общий профиль), НЕ трогая
    // отображаемую m_page: setHtml, ожидание loadFinished +
    // MathJax (poll простыми JS-выражениями, backstop mathjaxWaitSec),
    // window.fitWideMath() (callback, backstop 5s, task-12),
    // printToPdf внутри callback (backstop 30s), запись файла, deleteLater page.
    // Синхронно. Возвращает true, если файл успешно записан.
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
    bool m_initialized;
    bool m_pdfBusy;
};

#endif // WEBENGINEHOST_H
