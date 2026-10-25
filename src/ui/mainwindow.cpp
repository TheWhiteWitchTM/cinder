#include "ui/mainwindow.h"

#include "editor.h"
#include "lint.h"

#include <algorithm>

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSizePolicy>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStringDecoder>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

namespace {

bool detectTabs(const QString &text)
{
    int tabs = 0;
    int spaces = 0;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1Char('\t')))
            ++tabs;
        else if (line.startsWith(QLatin1String("    ")))
            ++spaces;
    }
    return tabs > spaces && tabs > 0;
}

QString displayName(const CodeEdit *edit)
{
    const QString path = edit->property("path").toString();
    if (!path.isEmpty())
        return QFileInfo(path).fileName();
    return edit->property("title").toString();
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Cinder"));
    resize(1200, 760);
    setAcceptDrops(true);
    buildUi();
    buildMenus();

    lintTimer_ = new QTimer(this);
    lintTimer_->setSingleShot(true);
    lintTimer_->setInterval(400);
    connect(lintTimer_, &QTimer::timeout, this, &MainWindow::updateLint);
    outlineTimer_ = new QTimer(this);
    outlineTimer_->setSingleShot(true);
    outlineTimer_->setInterval(200);
    connect(outlineTimer_, &QTimer::timeout, this, &MainWindow::updateOutline);

    shellcheck_ = new QProcess(this);
    connect(shellcheck_, &QProcess::started, this, [this] {
        shellcheck_->write(lintText_.toUtf8());
        shellcheck_->closeWriteChannel();
    });
    connect(shellcheck_, &QProcess::finished, this, [this](int, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit) {
            const QByteArray out = shellcheck_->readAllStandardOutput();
            if (!out.isEmpty() && currentEdit() && pathFor(currentEdit()) == lintPath_)
                showProblems(parseShellcheck(out, lintPath_));
        }
        if (lintAgain_)
            startShellcheck();
    });
    connect(shellcheck_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            shellcheckMissing_ = true;
            refreshStatus();
        }
    });

    connect(&runner_, &Runner::output, this, &MainWindow::onOutput);
    connect(&runner_, &Runner::started, this, [this](const QString &description) {
        note(description);
        refreshRunActions();
    });
    connect(&runner_, &Runner::traceLine, this, [this](const QString &file, int line) {
        if (CodeEdit *edit = editFor(file))
            edit->setExecLine(line);
    });
    connect(&runner_, &Runner::finished, this, &MainWindow::onSessionFinished);

    connect(&debugger_, &Debugger::output, this, &MainWindow::onOutput);
    connect(&debugger_, &Debugger::started, this, [this](const QString &description) {
        note(description);
        refreshRunActions();
    });
    connect(&debugger_, &Debugger::pausedAt, this, &MainWindow::onPaused);
    connect(&debugger_, &Debugger::failed, this, [this](const QString &message) { note(message); });
    connect(&debugger_, &Debugger::finished, this, &MainWindow::onSessionFinished);

    if (!settings_.geometry().isEmpty())
        restoreGeometry(settings_.geometry());
    if (!settings_.splitter().isEmpty())
        split_->restoreState(settings_.splitter());
    if (!settings_.bottomSplitter().isEmpty())
        centerSplit_->restoreState(settings_.bottomSplitter());
    const QString folder = settings_.folder().isEmpty() ? QDir::homePath() : settings_.folder();
    setFolder(folder);
    const int interp = interp_->findData(settings_.interpreter());
    if (interp >= 0)
        interp_->setCurrentIndex(interp);
    shellOnly_->setChecked(settings_.shellOnly());
    wrapAct_->setChecked(settings_.wrap());
    applyShellFilter();
    rebuildRecent();
    refreshRunActions();
    refreshStatus();

    server_ = new QLocalServer(this);
    QLocalServer::removeServer(QStringLiteral("cinder-editor"));
    server_->listen(QStringLiteral("cinder-editor"));
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket *socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::readyRead, socket, [socket] {
                socket->setProperty("buf", socket->property("buf").toByteArray() + socket->readAll());
            });
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                const QString data = QString::fromUtf8(socket->property("buf").toByteArray());
                const QStringList lines = data.split(QLatin1Char('\n'));
                for (const QString &line : lines) {
                    if (!line.isEmpty() && line != QLatin1String("RAISE"))
                        openPath(line);
                }
                raise();
                activateWindow();
                socket->deleteLater();
            });
        }
    });
}

void MainWindow::buildUi()
{
    files_ = new QFileSystemModel(this);
    files_->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::AllDirs);
    files_->setRootPath(QDir::homePath());
    tree_ = new QTreeView(this);
    tree_->setModel(files_);
    tree_->setHeaderHidden(true);
    for (int column = 1; column < files_->columnCount(); ++column)
        tree_->hideColumn(column);
    tree_->setAnimated(false);
    connect(tree_, &QTreeView::activated, this, [this](const QModelIndex &index) {
        if (!files_->isDir(index))
            openPath(files_->filePath(index));
    });

    shellOnly_ = new QCheckBox(QStringLiteral("Shell files only"), this);
    connect(shellOnly_, &QCheckBox::toggled, this, [this](bool on) {
        settings_.setShellOnly(on);
        applyShellFilter();
    });

    auto *side = new QWidget(this);
    side->setObjectName(QStringLiteral("sidebar"));
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(8, 8, 8, 8);
    sideLayout->addWidget(shellOnly_);
    sideLayout->addWidget(tree_, 1);

    tabs_ = new QTabWidget(this);
    tabs_->setDocumentMode(true);
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    connect(tabs_, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int) {
        refreshTitle();
        refreshStatus();
        refreshTabs();
        outlineTimer_->start();
        lintTimer_->start();
        if (CodeEdit *edit = currentEdit()) {
            const QString path = pathFor(edit);
            argsEdit_->setText(path.isEmpty() ? QString() : settings_.arguments(path));
            if (findEdit_)
                edit->setSearchNeedle(findEdit_->text());
        }
    });

    auto *welcome = new QWidget(this);
    welcome->setObjectName(QStringLiteral("welcome"));
    auto *welcomeLayout = new QVBoxLayout(welcome);
    welcomeLayout->setAlignment(Qt::AlignCenter);
    auto *word = new QLabel(QStringLiteral("Cinder"), welcome);
    word->setObjectName(QStringLiteral("wordmark"));
    QFont wordFont(QStringLiteral("DejaVu Serif"), 42);
    word->setFont(wordFont);
    word->setAlignment(Qt::AlignCenter);
    auto *tag = new QLabel(QStringLiteral("A black editor for bash and sh."), welcome);
    tag->setObjectName(QStringLiteral("tagline"));
    tag->setAlignment(Qt::AlignCenter);
    auto *hint = new QLabel(QStringLiteral("F5 runs  ·  F6 traces  ·  F7 debugs\nClick the gutter for a breakpoint. F10 steps over, F11 steps in."),
                            welcome);
    hint->setObjectName(QStringLiteral("hint"));
    hint->setAlignment(Qt::AlignCenter);
    auto *welcomeButtons = new QHBoxLayout;
    auto *bashButton = new QPushButton(QStringLiteral("New bash script"), welcome);
    bashButton->setObjectName(QStringLiteral("primary"));
    auto *shButton = new QPushButton(QStringLiteral("New sh script"), welcome);
    auto *openButton = new QPushButton(QStringLiteral("Open"), welcome);
    connect(bashButton, &QPushButton::clicked, this, [this] { newScript(Dialect::Bash); });
    connect(shButton, &QPushButton::clicked, this, [this] { newScript(Dialect::Sh); });
    connect(openButton, &QPushButton::clicked, this, &MainWindow::openFile);
    welcomeButtons->addStretch();
    welcomeButtons->addWidget(bashButton);
    welcomeButtons->addWidget(shButton);
    welcomeButtons->addWidget(openButton);
    welcomeButtons->addStretch();
    welcomeLayout->addWidget(word);
    welcomeLayout->addWidget(tag);
    welcomeLayout->addSpacing(8);
    welcomeLayout->addWidget(hint);
    welcomeLayout->addSpacing(18);
    welcomeLayout->addLayout(welcomeButtons);

    stack_ = new QStackedWidget(this);
    stack_->addWidget(welcome);
    stack_->addWidget(tabs_);

    findBar_ = new QWidget(this);
    auto *findLayout = new QHBoxLayout(findBar_);
    findLayout->setContentsMargins(8, 6, 8, 6);
    findEdit_ = new QLineEdit(findBar_);
    findEdit_->setPlaceholderText(QStringLiteral("Find"));
    replaceEdit_ = new QLineEdit(findBar_);
    replaceEdit_->setPlaceholderText(QStringLiteral("Replace"));
    auto *nextButton = new QPushButton(QStringLiteral("Next"), findBar_);
    auto *prevButton = new QPushButton(QStringLiteral("Previous"), findBar_);
    auto *replaceButton = new QPushButton(QStringLiteral("Replace"), findBar_);
    auto *allButton = new QPushButton(QStringLiteral("All"), findBar_);
    auto *closeFind = new QPushButton(QStringLiteral("Close"), findBar_);
    findLayout->addWidget(findEdit_, 1);
    findLayout->addWidget(replaceEdit_, 1);
    findLayout->addWidget(nextButton);
    findLayout->addWidget(prevButton);
    findLayout->addWidget(replaceButton);
    findLayout->addWidget(allButton);
    findLayout->addWidget(closeFind);
    findBar_->hide();
    connect(findEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (CodeEdit *edit = currentEdit())
            edit->setSearchNeedle(text);
    });
    connect(findEdit_, &QLineEdit::returnPressed, this, [this] { findNext(false); });
    connect(nextButton, &QPushButton::clicked, this, [this] { findNext(false); });
    connect(prevButton, &QPushButton::clicked, this, [this] { findNext(true); });
    connect(replaceButton, &QPushButton::clicked, this, &MainWindow::replaceOne);
    connect(allButton, &QPushButton::clicked, this, &MainWindow::replaceAll);
    connect(closeFind, &QPushButton::clicked, this, [this] {
        findBar_->hide();
        if (CodeEdit *edit = currentEdit()) {
            edit->setSearchNeedle(QString());
            edit->setFocus();
        }
    });

    auto *editorPane = new QWidget(this);
    auto *editorLayout = new QVBoxLayout(editorPane);
    editorLayout->setContentsMargins(0, 0, 0, 0);
    editorLayout->setSpacing(0);
    editorLayout->addWidget(stack_, 1);
    editorLayout->addWidget(findBar_);

    console_ = new Console(this);
    connect(console_, &Console::submitInput, this, [this](const QString &line) {
        if (runner_.running())
            runner_.writeInput(line);
        else if (debugger_.running())
            debugger_.writeInput(line);
    });
    problems_ = new QListWidget(this);
    connect(problems_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        jumpTo(item->data(Qt::UserRole).toInt(), item->data(Qt::UserRole + 1).toInt());
    });
    vars_ = new QTableWidget(0, 2, this);
    vars_->setHorizontalHeaderLabels({QStringLiteral("Name"), QStringLiteral("Value")});
    vars_->horizontalHeader()->setStretchLastSection(true);
    vars_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    vars_->verticalHeader()->hide();
    vars_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vars_->setSelectionBehavior(QAbstractItemView::SelectRows);
    vars_->setShowGrid(false);

    bottom_ = new QTabWidget(this);
    bottom_->addTab(console_, QStringLiteral("Console"));
    bottom_->addTab(problems_, QStringLiteral("Problems"));
    bottom_->addTab(vars_, QStringLiteral("Variables"));
    auto *bottomWrap = new QWidget(this);
    bottomWrap->setObjectName(QStringLiteral("bottom"));
    auto *bottomLayout = new QVBoxLayout(bottomWrap);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->addWidget(bottom_);

    outline_ = new QListWidget(this);
    connect(outline_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        jumpTo(item->data(Qt::UserRole).toInt(), 1);
    });
    auto *outlinePane = new QWidget(this);
    outlinePane->setObjectName(QStringLiteral("outlinePane"));
    auto *outlineLayout = new QVBoxLayout(outlinePane);
    outlineLayout->setContentsMargins(8, 8, 8, 8);
    auto *outlineLabel = new QLabel(QStringLiteral("Outline"), outlinePane);
    outlineLabel->setObjectName(QStringLiteral("section"));
    outlineLayout->addWidget(outlineLabel);
    outlineLayout->addWidget(outline_, 1);

    centerSplit_ = new QSplitter(Qt::Vertical, this);
    centerSplit_->addWidget(editorPane);
    centerSplit_->addWidget(bottomWrap);
    centerSplit_->setStretchFactor(0, 3);
    centerSplit_->setStretchFactor(1, 1);
    centerSplit_->setSizes({520, 200});

    split_ = new QSplitter(Qt::Horizontal, this);
    split_->addWidget(side);
    split_->addWidget(centerSplit_);
    split_->addWidget(outlinePane);
    split_->setStretchFactor(1, 1);
    split_->setSizes({240, 760, 200});

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("root"));
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(split_);
    setCentralWidget(central);

    statusDialect_ = new QLabel(this);
    statusPos_ = new QLabel(this);
    statusIndent_ = new QLabel(this);
    statusState_ = new QLabel(this);
    statusBar()->addWidget(statusDialect_);
    statusBar()->addWidget(statusPos_);
    statusBar()->addWidget(statusIndent_);
    statusBar()->addPermanentWidget(statusState_);
}

void MainWindow::buildMenus()
{
    auto *newBash = new QAction(QStringLiteral("New bash script"), this);
    newBash->setIconText(QStringLiteral("New"));
    newBash->setShortcut(QKeySequence::New);
    connect(newBash, &QAction::triggered, this, [this] { newScript(Dialect::Bash); });
    auto *newSh = new QAction(QStringLiteral("New sh script"), this);
    newSh->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_N);
    connect(newSh, &QAction::triggered, this, [this] { newScript(Dialect::Sh); });
    auto *openAct = new QAction(QStringLiteral("Open…"), this);
    openAct->setShortcut(QKeySequence::Open);
    connect(openAct, &QAction::triggered, this, &MainWindow::openFile);
    auto *openFolderAct = new QAction(QStringLiteral("Open folder…"), this);
    openFolderAct->setShortcut(Qt::CTRL | Qt::SHIFT | Qt::Key_O);
    connect(openFolderAct, &QAction::triggered, this, &MainWindow::openFolder);
    auto *saveAct = new QAction(QStringLiteral("Save"), this);
    saveAct->setShortcut(QKeySequence::Save);
    connect(saveAct, &QAction::triggered, this, &MainWindow::save);
    auto *saveAsAct = new QAction(QStringLiteral("Save as…"), this);
    saveAsAct->setShortcut(QKeySequence::SaveAs);
    connect(saveAsAct, &QAction::triggered, this, &MainWindow::saveAs);
    auto *saveAllAct = new QAction(QStringLiteral("Save all"), this);
    connect(saveAllAct, &QAction::triggered, this, &MainWindow::saveAll);
    auto *closeAct = new QAction(QStringLiteral("Close"), this);
    closeAct->setShortcut(QKeySequence::Close);
    connect(closeAct, &QAction::triggered, this, [this] { closeTab(tabs_->currentIndex()); });
    auto *execAct = new QAction(QStringLiteral("Make executable"), this);
    connect(execAct, &QAction::triggered, this, &MainWindow::makeExecutable);
    auto *quitAct = new QAction(QStringLiteral("Quit"), this);
    quitAct->setShortcut(QKeySequence::Quit);
    connect(quitAct, &QAction::triggered, this, &QWidget::close);

    recentMenu_ = new QMenu(QStringLiteral("Open recent"), this);
    auto *fileMenu = menuBar()->addMenu(QStringLiteral("File"));
    fileMenu->addAction(newBash);
    fileMenu->addAction(newSh);
    fileMenu->addSeparator();
    fileMenu->addAction(openAct);
    fileMenu->addAction(openFolderAct);
    fileMenu->addMenu(recentMenu_);
    fileMenu->addSeparator();
    fileMenu->addAction(saveAct);
    fileMenu->addAction(saveAsAct);
    fileMenu->addAction(saveAllAct);
    fileMenu->addAction(closeAct);
    fileMenu->addSeparator();
    fileMenu->addAction(execAct);
    fileMenu->addSeparator();
    fileMenu->addAction(quitAct);

    auto *undoAct = new QAction(QStringLiteral("Undo"), this);
    undoAct->setShortcut(QKeySequence::Undo);
    connect(undoAct, &QAction::triggered, this, [this] { if (currentEdit()) currentEdit()->undo(); });
    auto *redoAct = new QAction(QStringLiteral("Redo"), this);
    redoAct->setShortcut(QKeySequence::Redo);
    connect(redoAct, &QAction::triggered, this, [this] { if (currentEdit()) currentEdit()->redo(); });
    auto *cutAct = new QAction(QStringLiteral("Cut"), this);
    cutAct->setShortcut(QKeySequence::Cut);
    connect(cutAct, &QAction::triggered, this, [this] { if (currentEdit()) currentEdit()->cut(); });
    auto *copyAct = new QAction(QStringLiteral("Copy"), this);
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, this, [this] { if (currentEdit()) currentEdit()->copy(); });
    auto *pasteAct = new QAction(QStringLiteral("Paste"), this);
    pasteAct->setShortcut(QKeySequence::Paste);
    connect(pasteAct, &QAction::triggered, this, [this] { if (currentEdit()) currentEdit()->paste(); });
    auto *findAct = new QAction(QStringLiteral("Find"), this);
    findAct->setShortcut(QKeySequence::Find);
    connect(findAct, &QAction::triggered, this, [this] { showFind(false); });
    auto *replaceAct = new QAction(QStringLiteral("Replace"), this);
    replaceAct->setShortcut(QKeySequence::Replace);
    connect(replaceAct, &QAction::triggered, this, [this] { showFind(true); });
    auto *commentAct = new QAction(QStringLiteral("Toggle comment"), this);
    commentAct->setShortcut(Qt::CTRL | Qt::Key_Slash);
    connect(commentAct, &QAction::triggered, this, [this] { if (currentEdit()) currentEdit()->commentSelection(); });
    auto *gotoAct = new QAction(QStringLiteral("Go to line"), this);
    gotoAct->setShortcut(Qt::CTRL | Qt::Key_G);
    connect(gotoAct, &QAction::triggered, this, &MainWindow::gotoLine);
    auto *editMenu = menuBar()->addMenu(QStringLiteral("Edit"));
    editMenu->addAction(undoAct);
    editMenu->addAction(redoAct);
    editMenu->addSeparator();
    editMenu->addAction(cutAct);
    editMenu->addAction(copyAct);
    editMenu->addAction(pasteAct);
    editMenu->addSeparator();
    editMenu->addAction(findAct);
    editMenu->addAction(replaceAct);
    editMenu->addAction(commentAct);
    editMenu->addAction(gotoAct);

    runAct_ = new QAction(QStringLiteral("Run"), this);
    runAct_->setShortcut(Qt::Key_F5);
    connect(runAct_, &QAction::triggered, this, [this] {
        if (debugger_.paused())
            debugger_.cont();
        else if (!debugger_.running() && !runner_.running())
            run();
    });
    traceAct_ = new QAction(QStringLiteral("Trace"), this);
    traceAct_->setShortcut(Qt::Key_F6);
    connect(traceAct_, &QAction::triggered, this, &MainWindow::trace);
    debugAct_ = new QAction(QStringLiteral("Debug"), this);
    debugAct_->setShortcut(Qt::Key_F7);
    connect(debugAct_, &QAction::triggered, this, &MainWindow::debug);
    stopAct_ = new QAction(QStringLiteral("Stop"), this);
    stopAct_->setShortcut(Qt::SHIFT | Qt::Key_F5);
    connect(stopAct_, &QAction::triggered, this, &MainWindow::stop);
    pauseAct_ = new QAction(QStringLiteral("Pause"), this);
    pauseAct_->setShortcut(Qt::Key_F8);
    connect(pauseAct_, &QAction::triggered, this, [this] {
        if (debugger_.paused())
            debugger_.cont();
        else
            debugger_.requestPause();
    });
    nextAct_ = new QAction(QStringLiteral("Step over"), this);
    nextAct_->setIconText(QStringLiteral("Over"));
    nextAct_->setShortcut(Qt::Key_F10);
    connect(nextAct_, &QAction::triggered, this, [this] { debugger_.stepOver(); refreshRunActions(); });
    stepAct_ = new QAction(QStringLiteral("Step in"), this);
    stepAct_->setIconText(QStringLiteral("In"));
    stepAct_->setShortcut(Qt::Key_F11);
    connect(stepAct_, &QAction::triggered, this, [this] { debugger_.stepInto(); refreshRunActions(); });
    contAct_ = new QAction(QStringLiteral("Continue"), this);
    connect(contAct_, &QAction::triggered, this, [this] { debugger_.cont(); refreshRunActions(); });
    auto *breakAct = new QAction(QStringLiteral("Toggle breakpoint"), this);
    breakAct->setShortcut(Qt::Key_F9);
    connect(breakAct, &QAction::triggered, this, [this] {
        if (CodeEdit *edit = currentEdit())
            edit->toggleBreakpoint(edit->textCursor().blockNumber() + 1);
    });
    auto *clearBreaks = new QAction(QStringLiteral("Clear breakpoints"), this);
    connect(clearBreaks, &QAction::triggered, this, [this] {
        if (CodeEdit *edit = currentEdit())
            edit->setBreakpoints({});
    });
    auto *selectionAct = new QAction(QStringLiteral("Run selection"), this);
    connect(selectionAct, &QAction::triggered, this, &MainWindow::runSelection);
    auto *runMenu = menuBar()->addMenu(QStringLiteral("Run"));
    runMenu->addAction(runAct_);
    runMenu->addAction(traceAct_);
    runMenu->addAction(debugAct_);
    runMenu->addAction(stopAct_);
    runMenu->addAction(pauseAct_);
    runMenu->addSeparator();
    runMenu->addAction(contAct_);
    runMenu->addAction(nextAct_);
    runMenu->addAction(stepAct_);
    runMenu->addSeparator();
    runMenu->addAction(breakAct);
    runMenu->addAction(clearBreaks);
    runMenu->addSeparator();
    runMenu->addAction(selectionAct);

    wrapAct_ = new QAction(QStringLiteral("Word wrap"), this);
    wrapAct_->setCheckable(true);
    connect(wrapAct_, &QAction::toggled, this, [this](bool on) {
        settings_.setWrap(on);
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i)))
                edit->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
        }
    });
    auto *zoomIn = new QAction(QStringLiteral("Bigger text"), this);
    zoomIn->setShortcut(QKeySequence::ZoomIn);
    connect(zoomIn, &QAction::triggered, this, [this] {
        const int size = qMin(32, settings_.fontSize() + 1);
        settings_.setFontSize(size);
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i)))
                edit->setFontPointSize(size);
        }
    });
    auto *zoomOut = new QAction(QStringLiteral("Smaller text"), this);
    zoomOut->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOut, &QAction::triggered, this, [this] {
        const int size = qMax(8, settings_.fontSize() - 1);
        settings_.setFontSize(size);
        for (int i = 0; i < tabs_->count(); ++i) {
            if (auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i)))
                edit->setFontPointSize(size);
        }
    });
    auto *viewMenu = menuBar()->addMenu(QStringLiteral("View"));
    viewMenu->addAction(wrapAct_);
    viewMenu->addAction(zoomIn);
    viewMenu->addAction(zoomOut);

    auto *aboutAct = new QAction(QStringLiteral("About Cinder"), this);
    connect(aboutAct, &QAction::triggered, this, &MainWindow::about);
    menuBar()->addMenu(QStringLiteral("Help"))->addAction(aboutAct);

    auto *toolbar = addToolBar(QStringLiteral("Main"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(newBash);
    toolbar->addAction(openAct);
    toolbar->addAction(saveAct);
    toolbar->addSeparator();
    toolbar->addAction(runAct_);
    toolbar->addAction(traceAct_);
    toolbar->addAction(debugAct_);
    toolbar->addAction(stopAct_);
    toolbar->addSeparator();
    toolbar->addAction(nextAct_);
    toolbar->addAction(stepAct_);
    toolbar->addAction(contAct_);
    toolbar->addAction(pauseAct_);
    toolbar->addSeparator();
    argsEdit_ = new QLineEdit(toolbar);
    argsEdit_->setPlaceholderText(QStringLiteral("arguments"));
    argsEdit_->setClearButtonEnabled(true);
    argsEdit_->setMinimumWidth(140);
    argsEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    connect(argsEdit_, &QLineEdit::editingFinished, this, [this] {
        if (CodeEdit *edit = currentEdit()) {
            const QString path = pathFor(edit);
            if (!path.isEmpty())
                settings_.setArguments(path, argsEdit_->text());
        }
    });
    toolbar->addWidget(argsEdit_);
    interp_ = new QComboBox(toolbar);
    interp_->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    interp_->addItem(QStringLiteral("bash"), QStringLiteral("bash"));
    interp_->addItem(QStringLiteral("sh"), QStringLiteral("sh"));
    connect(interp_, &QComboBox::currentIndexChanged, this, [this](int) {
        settings_.setInterpreter(interp_->currentData().toString());
        refreshStatus();
    });
    toolbar->addWidget(interp_);
    for (QToolButton *button : toolbar->findChildren<QToolButton *>()) {
        if (button->defaultAction() == runAct_)
            button->setObjectName(QStringLiteral("primary"));
    }
}

CodeEdit *MainWindow::createEdit()
{
    auto *edit = new CodeEdit(this);
    edit->setFontPointSize(settings_.fontSize());
    edit->setTabWidth(settings_.tabWidth());
    edit->setLineWrapMode(settings_.wrap() ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    connect(edit, &CodeEdit::cursorChanged, this, [this, edit](int, int) {
        if (edit == currentEdit())
            refreshStatus();
    });
    connect(edit, &CodeEdit::breakpointsChanged, this, [this, edit] {
        const QString path = pathFor(edit);
        if (!path.isEmpty())
            settings_.setBreakpoints(path, edit->breakpoints());
        if (debugger_.running())
            debugger_.setBreakpoints(allBreakpoints());
        refreshStatus();
    });
    connect(edit->document(), &QTextDocument::modificationChanged, this, [this] {
        refreshTabs();
        refreshTitle();
    });
    connect(edit, &QPlainTextEdit::textChanged, this, [this, edit] {
        if (loading_)
            return;
        if (edit->execLine() != 0 && !debugger_.paused() && !runner_.running())
            edit->setExecLine(0);
        if (edit == currentEdit()) {
            lintTimer_->start();
            outlineTimer_->start();
        }
    });
    return edit;
}

CodeEdit *MainWindow::currentEdit() const
{
    return qobject_cast<CodeEdit *>(tabs_->currentWidget());
}

CodeEdit *MainWindow::editFor(const QString &path) const
{
    if (path.isEmpty())
        return nullptr;
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i));
        if (edit && pathFor(edit) == abs)
            return edit;
    }
    return nullptr;
}

QString MainWindow::pathFor(const CodeEdit *edit) const
{
    return edit ? edit->property("path").toString() : QString();
}

void MainWindow::openPaths(const QStringList &paths)
{
    for (const QString &path : paths)
        openPath(path);
}

void MainWindow::openPath(const QString &path)
{
    const QFileInfo info(path);
    if (info.isDir()) {
        setFolder(info.absoluteFilePath());
        return;
    }
    const QString abs = info.absoluteFilePath();
    if (CodeEdit *existing = editFor(abs)) {
        tabs_->setCurrentWidget(existing);
        stack_->setCurrentIndex(1);
        return;
    }
    if (!info.exists() || !info.isFile()) {
        note(QStringLiteral("No such file: ") + path);
        return;
    }
    if (info.size() > 5 * 1024 * 1024) {
        note(QStringLiteral("That file is too large for Cinder."));
        return;
    }
    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly)) {
        note(QStringLiteral("Could not open ") + abs);
        return;
    }
    const QByteArray bytes = file.readAll();
    if (looksBinary(bytes)) {
        note(QStringLiteral("That file is not text."));
        return;
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    QString text = decoder(bytes);
    bool latin = false;
    if (decoder.hasError()) {
        text = QString::fromLatin1(bytes);
        latin = true;
    }
    const bool crlf = text.contains(QLatin1String("\r\n")) || text.contains(QLatin1Char('\r'));
    text.replace(QLatin1String("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QStringLiteral("\n"));

    loading_ = true;
    CodeEdit *edit = createEdit();
    edit->setProperty("path", abs);
    edit->setIndentTabs(detectTabs(text));
    edit->setDialect(detectDialect(abs, text));
    edit->setPlainText(text);
    edit->setBreakpoints(settings_.breakpoints(abs));
    edit->document()->setModified(crlf);
    loading_ = false;

    tabs_->addTab(edit, info.fileName());
    tabs_->setCurrentWidget(edit);
    stack_->setCurrentIndex(1);
    settings_.rememberFile(abs);
    if (settings_.folder().isEmpty())
        setFolder(info.absolutePath());
    rebuildRecent();
    refreshTabs();
    refreshTitle();
    if (crlf)
        note(QStringLiteral("Converted CRLF to LF. Save to keep it."));
    if (latin)
        note(QStringLiteral("Opened as Latin-1. Save writes UTF-8."));
    outlineTimer_->start();
    lintTimer_->start();
}

void MainWindow::newScript(Dialect dialect)
{
    loading_ = true;
    CodeEdit *edit = createEdit();
    const QString title = dialect == Dialect::Bash ? QStringLiteral("untitled.bash") : QStringLiteral("untitled.sh");
    const QString body = dialect == Dialect::Bash
        ? QStringLiteral("#!/usr/bin/env bash\nset -euo pipefail\n\n")
        : QStringLiteral("#!/bin/sh\nset -eu\n\n");
    edit->setProperty("title", title);
    edit->setDialect(dialect);
    edit->setPlainText(body);
    edit->moveCursor(QTextCursor::End);
    edit->document()->setModified(true);
    loading_ = false;
    tabs_->addTab(edit, title);
    tabs_->setCurrentWidget(edit);
    stack_->setCurrentIndex(1);
    edit->setFocus();
    refreshTabs();
    refreshTitle();
    lintTimer_->start();
}

void MainWindow::openFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open script"),
        settings_.folder().isEmpty() ? QDir::homePath() : settings_.folder(),
        QStringLiteral("Shell scripts (*.sh *.bash *.bats);;All files (*)"));
    if (!path.isEmpty())
        openPath(path);
}

void MainWindow::openFolder()
{
    const QString path = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Open folder"),
        settings_.folder().isEmpty() ? QDir::homePath() : settings_.folder());
    if (!path.isEmpty())
        setFolder(path);
}

bool MainWindow::save()
{
    CodeEdit *edit = currentEdit();
    if (!edit)
        return false;
    if (pathFor(edit).isEmpty())
        return saveAs();
    const QString path = pathFor(edit);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        note(QStringLiteral("Could not save ") + path);
        return false;
    }
    QString text = edit->toPlainText();
    if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
        text += QLatin1Char('\n');
    file.write(text.toUtf8());
    file.close();
    edit->document()->setModified(false);
    settings_.rememberFile(path);
    rebuildRecent();
    refreshTabs();
    refreshTitle();
    note(QStringLiteral("Saved ") + path);
    return true;
}

bool MainWindow::saveAs()
{
    CodeEdit *edit = currentEdit();
    if (!edit)
        return false;
    QString suggested = pathFor(edit);
    if (suggested.isEmpty()) {
        suggested = QDir(settings_.folder().isEmpty() ? QDir::homePath() : settings_.folder())
                        .filePath(edit->property("title").toString());
    }
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save script"), suggested,
        QStringLiteral("Shell scripts (*.sh *.bash);;All files (*)"));
    if (path.isEmpty())
        return false;
    edit->setProperty("path", QFileInfo(path).absoluteFilePath());
    edit->setDialect(detectDialect(path, edit->toPlainText()));
    return save();
}

void MainWindow::saveAll()
{
    const int current = tabs_->currentIndex();
    for (int i = 0; i < tabs_->count(); ++i) {
        tabs_->setCurrentIndex(i);
        if (auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i))) {
            if (edit->document()->isModified() || pathFor(edit).isEmpty()) {
                if (!save())
                    break;
            }
        }
    }
    tabs_->setCurrentIndex(current);
}

void MainWindow::closeTab(int index)
{
    auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(index));
    if (!edit)
        return;
    if (edit->document()->isModified()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Cinder"),
            QStringLiteral("Save changes to %1?").arg(displayName(edit)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Save) {
            tabs_->setCurrentIndex(index);
            if (!save())
                return;
        }
    }
    tabs_->removeTab(index);
    edit->deleteLater();
    if (tabs_->count() == 0)
        stack_->setCurrentIndex(0);
    refreshTitle();
    refreshStatus();
}

void MainWindow::makeExecutable()
{
    CodeEdit *edit = currentEdit();
    if (!edit || pathFor(edit).isEmpty()) {
        note(QStringLiteral("Save the script first."));
        return;
    }
    const QString path = pathFor(edit);
    const QFile::Permissions now = QFile::permissions(path);
    if (!QFile::setPermissions(path, now | QFileDevice::ExeUser | QFileDevice::ExeGroup | QFileDevice::ExeOther)) {
        note(QStringLiteral("Could not mark it executable."));
        return;
    }
    note(QStringLiteral("Marked executable: ") + path);
}

bool MainWindow::prepareForRun()
{
    CodeEdit *edit = currentEdit();
    if (!edit) {
        note(QStringLiteral("Open a script first."));
        return false;
    }
    if (pathFor(edit).isEmpty()) {
        note(QStringLiteral("Save the script before running it."));
        return saveAs();
    }
    if (edit->document()->isModified() && !save())
        return false;
    return true;
}

QStringList MainWindow::currentArgs(bool *ok) const
{
    return splitArgs(argsEdit_->text(), ok);
}

MainWindow::Launch MainWindow::launchFor(const CodeEdit *edit, RunMode mode) const
{
    Launch launch;
    const QString choice = interp_->currentData().toString();
    const Shebang shebang = parseShebang(edit->toPlainText());
    QString program;
    QStringList extra = shebang.extra;
    if (choice == QLatin1String("bash"))
        program = findProgram(QStringLiteral("bash"));
    else if (choice == QLatin1String("sh"))
        program = findProgram(QStringLiteral("sh"));
    else if (!shebang.program.isEmpty())
        program = findProgram(shebang.program);
    else
        program = findProgram(edit->dialect() == Dialect::Bash ? QStringLiteral("bash") : QStringLiteral("sh"));

    const QString base = QFileInfo(program).fileName();
    const bool posix = base == QLatin1String("sh") || base == QLatin1String("dash") || base == QLatin1String("ash");
    if (mode == RunMode::Trace) {
        launch.program = findProgram(QStringLiteral("bash"));
        if (posix) {
            launch.prefix = {QStringLiteral("--posix"), QStringLiteral("-x")};
            launch.note = QStringLiteral("Tracing with bash --posix -x. Line numbers come from bash.");
        } else {
            launch.prefix = {QStringLiteral("-x")};
            launch.prefix.append(extra);
            launch.note = QStringLiteral("Tracing with bash -x.");
        }
        return launch;
    }
    launch.program = program;
    launch.prefix = extra;
    return launch;
}

QList<QPair<QString, int>> MainWindow::allBreakpoints() const
{
    QList<QPair<QString, int>> out;
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i));
        if (!edit)
            continue;
        const QString path = pathFor(edit);
        if (path.isEmpty())
            continue;
        QList<int> lines = edit->breakpoints().values();
        std::sort(lines.begin(), lines.end());
        for (int line : lines)
            out.append({path, line});
    }
    return out;
}

void MainWindow::run()
{
    if (!prepareForRun())
        return;
    CodeEdit *edit = currentEdit();
    bool ok = true;
    const QStringList args = currentArgs(&ok);
    if (!ok) {
        note(QStringLiteral("The arguments have an unclosed quote."));
        return;
    }
    clearExecLines();
    console_->clearAll();
    bottom_->setCurrentIndex(0);
    const Launch launch = launchFor(edit, RunMode::Run);
    RunSpec spec;
    spec.program = launch.program;
    spec.prefix = launch.prefix;
    spec.script = pathFor(edit);
    spec.args = args;
    spec.workDir = QFileInfo(spec.script).absolutePath();
    spec.mode = RunMode::Run;
    spec.dialect = edit->dialect();
    console_->setInputEnabled(true);
    runner_.start(spec);
    refreshRunActions();
}

void MainWindow::trace()
{
    if (!prepareForRun())
        return;
    CodeEdit *edit = currentEdit();
    bool ok = true;
    const QStringList args = currentArgs(&ok);
    if (!ok) {
        note(QStringLiteral("The arguments have an unclosed quote."));
        return;
    }
    clearExecLines();
    console_->clearAll();
    bottom_->setCurrentIndex(0);
    const Launch launch = launchFor(edit, RunMode::Trace);
    if (!launch.note.isEmpty())
        note(launch.note);
    RunSpec spec;
    spec.program = launch.program;
    spec.prefix = launch.prefix;
    spec.script = pathFor(edit);
    spec.args = args;
    spec.workDir = QFileInfo(spec.script).absolutePath();
    spec.mode = RunMode::Trace;
    spec.dialect = edit->dialect();
    console_->setInputEnabled(true);
    runner_.start(spec);
    refreshRunActions();
}

void MainWindow::debug()
{
    if (!prepareForRun())
        return;
    CodeEdit *edit = currentEdit();
    bool ok = true;
    const QStringList args = currentArgs(&ok);
    if (!ok) {
        note(QStringLiteral("The arguments have an unclosed quote."));
        return;
    }
    clearExecLines();
    console_->clearAll();
    bottom_->setCurrentIndex(0);
    vars_->setRowCount(0);
    if (edit->dialect() == Dialect::Sh)
        note(QStringLiteral("Debugging with bash. dash has no breakpoint trap."));
    DebugSpec spec;
    spec.script = pathFor(edit);
    spec.args = args;
    spec.workDir = QFileInfo(spec.script).absolutePath();
    spec.breakpoints = allBreakpoints();
    spec.breakOnEntry = spec.breakpoints.isEmpty();
    console_->setInputEnabled(true);
    debugger_.start(spec);
    refreshRunActions();
}

void MainWindow::stop()
{
    if (runner_.running())
        runner_.stop();
    if (debugger_.running())
        debugger_.stop();
    refreshRunActions();
}

void MainWindow::runSelection()
{
    CodeEdit *edit = currentEdit();
    if (!edit)
        return;
    QString selection = edit->textCursor().selectedText();
    selection.replace(QChar(0x2029), QLatin1Char('\n'));
    if (selection.trimmed().isEmpty()) {
        note(QStringLiteral("Select the lines to run."));
        return;
    }
    bool ok = true;
    const QStringList args = currentArgs(&ok);
    if (!ok) {
        note(QStringLiteral("The arguments have an unclosed quote."));
        return;
    }
    console_->clearAll();
    bottom_->setCurrentIndex(0);
    const Launch launch = launchFor(edit, RunMode::Run);
    RunSpec spec;
    spec.program = launch.program;
    spec.prefix = launch.prefix;
    spec.script = pathFor(edit);
    spec.args = args;
    spec.workDir = spec.script.isEmpty() ? QDir::currentPath() : QFileInfo(spec.script).absolutePath();
    spec.mode = RunMode::Run;
    spec.dialect = edit->dialect();
    spec.selection = selection;
    console_->setInputEnabled(true);
    runner_.start(spec);
    refreshRunActions();
}

void MainWindow::setFolder(const QString &path)
{
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return;
    settings_.setFolder(path);
    files_->setRootPath(path);
    tree_->setRootIndex(files_->index(path));
}

void MainWindow::applyShellFilter()
{
    if (shellOnly_->isChecked()) {
        files_->setNameFilters({QStringLiteral("*.sh"), QStringLiteral("*.bash"), QStringLiteral("*.bats"),
                                QStringLiteral("*.ksh")});
        files_->setNameFilterDisables(false);
    } else {
        files_->setNameFilters({});
    }
}

void MainWindow::rebuildRecent()
{
    recentMenu_->clear();
    const QStringList files = settings_.recentFiles();
    if (files.isEmpty()) {
        recentMenu_->addAction(QStringLiteral("Nothing yet"))->setEnabled(false);
        return;
    }
    for (const QString &path : files)
        recentMenu_->addAction(path, this, [this, path] { openPath(path); });
}

void MainWindow::refreshTabs()
{
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i));
        if (!edit)
            continue;
        QString name = displayName(edit);
        if (edit->document()->isModified())
            name = QStringLiteral("• ") + name;
        tabs_->setTabText(i, name);
        tabs_->setTabToolTip(i, pathFor(edit));
    }
}

void MainWindow::refreshTitle()
{
    CodeEdit *edit = currentEdit();
    if (!edit || stack_->currentIndex() == 0) {
        setWindowTitle(QStringLiteral("Cinder"));
        return;
    }
    QString name = displayName(edit);
    if (edit->document()->isModified())
        name += QStringLiteral(" •");
    setWindowTitle(name + QStringLiteral(" — Cinder"));
}

void MainWindow::refreshStatus()
{
    CodeEdit *edit = currentEdit();
    if (!edit) {
        statusDialect_->setText(QStringLiteral("Cinder"));
        statusPos_->clear();
        statusIndent_->clear();
    } else {
        const QTextCursor cursor = edit->textCursor();
        statusDialect_->setText(dialectName(edit->dialect()));
        statusPos_->setText(QStringLiteral("Ln %1, Col %2").arg(cursor.blockNumber() + 1).arg(cursor.positionInBlock() + 1));
        statusIndent_->setText(edit->indentTabs()
                                   ? QStringLiteral("Tabs")
                                   : QStringLiteral("Spaces: %1").arg(edit->tabWidth()));
    }
    QString state;
    if (debugger_.paused())
        state = QStringLiteral("Paused");
    else if (debugger_.running())
        state = QStringLiteral("Debugging");
    else if (runner_.running())
        state = QStringLiteral("Running");
    else if (shellcheckMissing_)
        state = QStringLiteral("shellcheck not installed");
    else
        state = QStringLiteral("Ready");
    if (edit)
        state += QStringLiteral("  ·  %1 breakpoint%2")
                     .arg(edit->breakpoints().size())
                     .arg(edit->breakpoints().size() == 1 ? QString() : QStringLiteral("s"));
    statusState_->setText(state);
}

void MainWindow::refreshRunActions()
{
    const bool running = runner_.running() || debugger_.running();
    const bool paused = debugger_.paused();
    runAct_->setEnabled(!debugger_.running() || paused);
    runAct_->setText(paused ? QStringLiteral("Continue") : QStringLiteral("Run"));
    traceAct_->setEnabled(!running);
    debugAct_->setEnabled(!running);
    stopAct_->setEnabled(running);
    stepAct_->setEnabled(paused);
    nextAct_->setEnabled(paused);
    contAct_->setEnabled(paused);
    pauseAct_->setEnabled(debugger_.running());
    console_->setInputEnabled(running);
    refreshStatus();
}

void MainWindow::updateOutline()
{
    outline_->clear();
    CodeEdit *edit = currentEdit();
    if (!edit)
        return;
    static const QRegularExpression withParen(
        QStringLiteral("^\\s*(?:function\\s+)?([A-Za-z_][A-Za-z0-9_]*)\\s*\\(\\s*\\)"));
    static const QRegularExpression keyword(
        QStringLiteral("^\\s*function\\s+([A-Za-z_][A-Za-z0-9_]*)\\b"));
    const QStringList lines = edit->toPlainText().split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size() && outline_->count() < 500; ++i) {
        QRegularExpressionMatch match = withParen.match(lines.at(i));
        if (!match.hasMatch())
            match = keyword.match(lines.at(i));
        if (!match.hasMatch())
            continue;
        auto *item = new QListWidgetItem(match.captured(1));
        item->setData(Qt::UserRole, i + 1);
        outline_->addItem(item);
    }
}

void MainWindow::updateLint()
{
    CodeEdit *edit = currentEdit();
    if (!edit) {
        showProblems({});
        return;
    }
    lintPath_ = pathFor(edit);
    lintText_ = edit->toPlainText();
    lintDialect_ = edit->dialect();
    showProblems(builtinLint(lintPath_, lintText_, lintDialect_));
    if (shellcheckMissing_)
        return;
    startShellcheck();
}

void MainWindow::startShellcheck()
{
    if (shellcheckMissing_ || lintText_.isEmpty()) {
        lintAgain_ = false;
        return;
    }
    if (shellcheck_->state() != QProcess::NotRunning) {
        lintAgain_ = true;
        return;
    }
    lintAgain_ = false;
    const QString shell = lintDialect_ == Dialect::Bash ? QStringLiteral("bash") : QStringLiteral("sh");
    shellcheck_->start(QStringLiteral("shellcheck"),
                       {QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("-s"), shell, QStringLiteral("-")});
}

void MainWindow::showProblems(const QList<Problem> &problems)
{
    problems_->clear();
    for (const Problem &problem : problems) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1:%2  %3").arg(problem.line).arg(problem.column).arg(problem.message));
        item->setData(Qt::UserRole, problem.line);
        item->setData(Qt::UserRole + 1, problem.column);
        QColor color(QStringLiteral("#8a8a8a"));
        if (problem.level == QLatin1String("error"))
            color = QColor(QStringLiteral("#ff7b72"));
        else if (problem.level == QLatin1String("warning"))
            color = QColor(QStringLiteral("#ffb454"));
        item->setForeground(color);
        problems_->addItem(item);
    }
    bottom_->setTabText(1, problems.isEmpty() ? QStringLiteral("Problems")
                                              : QStringLiteral("Problems (%1)").arg(problems.size()));
}

void MainWindow::showFind(bool replace)
{
    if (!currentEdit())
        return;
    findBar_->show();
    if (replace)
        replaceEdit_->setFocus();
    else
        findEdit_->setFocus();
    findEdit_->selectAll();
    if (CodeEdit *edit = currentEdit()) {
        if (findEdit_->text().isEmpty() && edit->textCursor().hasSelection())
            findEdit_->setText(edit->textCursor().selectedText().replace(QChar(0x2029), QString()));
        edit->setSearchNeedle(findEdit_->text());
    }
}

void MainWindow::findNext(bool backward)
{
    CodeEdit *edit = currentEdit();
    if (!edit || findEdit_->text().isEmpty())
        return;
    QTextDocument::FindFlags flags;
    if (backward)
        flags |= QTextDocument::FindBackward;
    if (!edit->find(findEdit_->text(), flags)) {
        QTextCursor cursor = edit->textCursor();
        cursor.movePosition(backward ? QTextCursor::End : QTextCursor::Start);
        edit->setTextCursor(cursor);
        if (!edit->find(findEdit_->text(), flags))
            note(QStringLiteral("No matches."));
    }
}

void MainWindow::replaceOne()
{
    CodeEdit *edit = currentEdit();
    if (!edit || findEdit_->text().isEmpty())
        return;
    QTextCursor cursor = edit->textCursor();
    if (cursor.hasSelection() && cursor.selectedText() == findEdit_->text()) {
        cursor.insertText(replaceEdit_->text());
    }
    findNext(false);
}

void MainWindow::replaceAll()
{
    CodeEdit *edit = currentEdit();
    if (!edit || findEdit_->text().isEmpty())
        return;
    QTextCursor cursor(edit->document());
    cursor.beginEditBlock();
    int count = 0;
    while (true) {
        cursor = edit->document()->find(findEdit_->text(), cursor);
        if (cursor.isNull())
            break;
        cursor.insertText(replaceEdit_->text());
        ++count;
    }
    cursor.endEditBlock();
    note(QStringLiteral("Replaced %1.").arg(count));
}

void MainWindow::gotoLine()
{
    CodeEdit *edit = currentEdit();
    if (!edit)
        return;
    bool ok = false;
    const int line = QInputDialog::getInt(this, QStringLiteral("Go to line"), QStringLiteral("Line"),
                                          edit->textCursor().blockNumber() + 1, 1, qMax(1, edit->blockCount()), 1, &ok);
    if (ok)
        jumpTo(line, 1);
}

void MainWindow::jumpTo(int line, int column)
{
    CodeEdit *edit = currentEdit();
    if (!edit || line < 1)
        return;
    QTextBlock block = edit->document()->findBlockByNumber(line - 1);
    if (!block.isValid())
        return;
    const int offset = qBound(0, column - 1, qMax(0, block.length() - 1));
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + offset);
    edit->setTextCursor(cursor);
    edit->setFocus();
    stack_->setCurrentIndex(1);
}

void MainWindow::about()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("About Cinder"));
    box.setText(QStringLiteral("Cinder"));
    box.setInformativeText(QStringLiteral(
        "A black editor for bash and sh.\n\n"
        "Run, trace, and debug scripts without leaving the window.\n"
        "MIT License.\n\n"
        "Mira, The White Witch"));
    box.exec();
}

void MainWindow::note(const QString &text)
{
    console_->appendStream(QStringLiteral("cinder: ") + text + QLatin1Char('\n'), kStreamNote);
    statusState_->setText(text);
}

void MainWindow::onOutput(const QString &text, int channel)
{
    console_->appendStream(text, channel);
}

void MainWindow::onPaused(const QString &file, int line, const QList<Var> &vars)
{
    clearExecLines();
    if (!file.isEmpty() && QFileInfo::exists(file))
        openPath(file);
    if (CodeEdit *edit = editFor(file)) {
        tabs_->setCurrentWidget(edit);
        edit->setExecLine(line);
    }
    vars_->setRowCount(vars.size());
    for (int i = 0; i < vars.size(); ++i) {
        vars_->setItem(i, 0, new QTableWidgetItem(vars.at(i).name));
        vars_->setItem(i, 1, new QTableWidgetItem(vars.at(i).value));
    }
    if (vars.isEmpty()) {
        vars_->setRowCount(1);
        auto *item = new QTableWidgetItem(QStringLiteral("(no new variables yet)"));
        item->setFlags(Qt::NoItemFlags);
        vars_->setItem(0, 0, item);
        vars_->setSpan(0, 0, 1, 2);
    }
    note(QStringLiteral("Paused at %1:%2").arg(QFileInfo(file).fileName()).arg(line));
    refreshRunActions();
}

void MainWindow::onSessionFinished(int code)
{
    clearExecLines();
    console_->setInputEnabled(false);
    refreshRunActions();
    if (code < 0)
        note(QStringLiteral("Stopped."));
    else
        note(QStringLiteral("Exit %1.").arg(code));
}

void MainWindow::clearExecLines()
{
    for (int i = 0; i < tabs_->count(); ++i) {
        if (auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i))) {
            if (edit->execLine() != 0)
                edit->setExecLine(0);
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (int i = 0; i < tabs_->count(); ++i) {
        auto *edit = qobject_cast<CodeEdit *>(tabs_->widget(i));
        if (!edit || !edit->document()->isModified())
            continue;
        tabs_->setCurrentIndex(i);
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Cinder"),
            QStringLiteral("Save changes to %1?").arg(displayName(edit)),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
        if (answer == QMessageBox::Save && !save()) {
            event->ignore();
            return;
        }
    }
    if (runner_.running())
        runner_.stop();
    if (debugger_.running())
        debugger_.stop();
    settings_.setGeometry(saveGeometry());
    settings_.setSplitter(split_->saveState());
    settings_.setBottomSplitter(centerSplit_->saveState());
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile())
            paths.append(url.toLocalFile());
    }
    openPaths(paths);
    event->acceptProposedAction();
}
