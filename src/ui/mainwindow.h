#pragma once

#include "common.h"
#include "console.h"
#include "debugger.h"
#include "runner.h"
#include "settings.h"

#include <QMainWindow>
#include <QPair>

class CodeEdit;
class QAction;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QFileSystemModel;
class QLabel;
class QLineEdit;
class QListWidget;
class QLocalServer;
class QMenu;
class QProcess;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QTabWidget;
class QTimer;
class QTreeView;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    void openPath(const QString &path);
    void openPaths(const QStringList &paths);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    struct Launch {
        QString program;
        QStringList prefix;
        QString note;
    };

    void buildUi();
    void buildMenus();
    CodeEdit *createEdit();
    CodeEdit *currentEdit() const;
    CodeEdit *editFor(const QString &path) const;
    QString pathFor(const CodeEdit *edit) const;

    void newScript(Dialect dialect);
    void openFile();
    void openFolder();
    bool save();
    bool saveAs();
    void saveAll();
    void closeTab(int index);
    void makeExecutable();
    bool prepareForRun();

    void run();
    void trace();
    void debug();
    void stop();
    void runSelection();
    Launch launchFor(const CodeEdit *edit, RunMode mode) const;
    QStringList currentArgs(bool *ok) const;
    QList<QPair<QString, int>> allBreakpoints() const;

    void setFolder(const QString &path);
    void applyShellFilter();
    void rebuildRecent();
    void refreshTabs();
    void refreshTitle();
    void refreshStatus();
    void refreshRunActions();
    void updateOutline();
    void updateLint();
    void startShellcheck();
    void showProblems(const QList<Problem> &problems);
    void showFind(bool replace);
    void findNext(bool backward);
    void replaceOne();
    void replaceAll();
    void gotoLine();
    void jumpTo(int line, int column);
    void about();

    void note(const QString &text);
    void onOutput(const QString &text, int channel);
    void onPaused(const QString &file, int line, const QList<Var> &vars);
    void onSessionFinished(int code);
    void clearExecLines();

    Settings settings_;
    Runner runner_;
    Debugger debugger_;
    QProcess *shellcheck_ = nullptr;
    bool shellcheckMissing_ = false;
    bool lintAgain_ = false;
    QString lintText_;
    QString lintPath_;
    Dialect lintDialect_ = Dialect::Bash;
    bool loading_ = false;

    QFileSystemModel *files_ = nullptr;
    QTreeView *tree_ = nullptr;
    QCheckBox *shellOnly_ = nullptr;
    QSplitter *split_ = nullptr;
    QSplitter *centerSplit_ = nullptr;
    QStackedWidget *stack_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QTabWidget *bottom_ = nullptr;
    Console *console_ = nullptr;
    QListWidget *problems_ = nullptr;
    QTableWidget *vars_ = nullptr;
    QListWidget *outline_ = nullptr;
    QLineEdit *argsEdit_ = nullptr;
    QComboBox *interp_ = nullptr;
    QWidget *findBar_ = nullptr;
    QLineEdit *findEdit_ = nullptr;
    QLineEdit *replaceEdit_ = nullptr;
    QMenu *recentMenu_ = nullptr;

    QAction *runAct_ = nullptr;
    QAction *traceAct_ = nullptr;
    QAction *debugAct_ = nullptr;
    QAction *stopAct_ = nullptr;
    QAction *stepAct_ = nullptr;
    QAction *nextAct_ = nullptr;
    QAction *contAct_ = nullptr;
    QAction *pauseAct_ = nullptr;
    QAction *wrapAct_ = nullptr;

    QLabel *statusDialect_ = nullptr;
    QLabel *statusPos_ = nullptr;
    QLabel *statusIndent_ = nullptr;
    QLabel *statusState_ = nullptr;
    QTimer *lintTimer_ = nullptr;
    QTimer *outlineTimer_ = nullptr;
    QLocalServer *server_ = nullptr;
};
