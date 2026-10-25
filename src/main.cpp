#include "selftest.h"
#include "ui/mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QIcon>
#include <QLocalSocket>
#include <QStyleFactory>

namespace {

void applyTheme(QApplication &app)
{
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion")))
        app.setStyle(fusion);
    QFont ui(QStringLiteral("Fira Sans"), 11);
    if (!QFontInfo(ui).exactMatch())
        ui = QFont(QStringLiteral("DejaVu Sans"), 11);
    app.setFont(ui);

    QPalette pal;
    const QColor black(QStringLiteral("#000000"));
    const QColor text(QStringLiteral("#e8e6e3"));
    const QColor ember(QStringLiteral("#ffb454"));
    pal.setColor(QPalette::Window, black);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, black);
    pal.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#0e0e0e")));
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::Button, QColor(QStringLiteral("#141414")));
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::Highlight, ember);
    pal.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#1a1206")));
    pal.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#141414")));
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::PlaceholderText, QColor(QStringLiteral("#6a6a6a")));
    pal.setColor(QPalette::BrightText, ember);
    pal.setColor(QPalette::Link, ember);
    app.setPalette(pal);

    QFile file(QStringLiteral(":/style.qss"));
    if (file.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(file.readAll()));
    const QIcon icon(QStringLiteral(":/icon-512.png"));
    if (!icon.isNull())
        app.setWindowIcon(icon);
}

bool handOff(const QStringList &paths)
{
    QLocalSocket socket;
    socket.connectToServer(QStringLiteral("cinder-editor"));
    if (!socket.waitForConnected(300))
        return false;
    QStringList lines;
    lines << QStringLiteral("RAISE");
    lines << paths;
    socket.write(lines.join(QLatin1Char('\n')).toUtf8());
    socket.flush();
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    bool selfTest = false;
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--self-test") == 0)
            selfTest = true;
    }
    if (selfTest)
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));

    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("cinder"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("cinder.local"));
    QCoreApplication::setApplicationName(QStringLiteral("cinder"));
    QApplication::setApplicationDisplayName(QStringLiteral("Cinder"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));
    applyTheme(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A black editor for bash and sh."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(QCommandLineOption(QStringList{QStringLiteral("self-test")},
                                        QStringLiteral("Run the built-in checks and exit.")));
    parser.addPositionalArgument(QStringLiteral("paths"), QStringLiteral("Scripts or folders to open."));
    parser.process(app);
    if (parser.isSet(QStringLiteral("self-test")))
        return runSelfTest();

    const QStringList paths = parser.positionalArguments();
    if (handOff(paths))
        return 0;

    MainWindow window;
    window.openPaths(paths);
    window.show();
    return app.exec();
}
