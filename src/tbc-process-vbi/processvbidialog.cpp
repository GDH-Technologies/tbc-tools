/******************************************************************************
 * processvbidialog.cpp
 * tbc-process-vbi - standalone VBI/VITS processing GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Harry Munday
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#include "processvbidialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString quoteForShell(const QString &arg)
{
    if (arg.isEmpty()) {
        return QStringLiteral("''");
    }
    QString escaped = arg;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
    return QStringLiteral("'%1'").arg(escaped);
}

QString chooseStartDirectory(const QString &preferredDirectory)
{
    const QString normalizedPreferred = QDir::cleanPath(preferredDirectory.trimmed());
    if (!normalizedPreferred.isEmpty() && QFileInfo::exists(normalizedPreferred)) {
        return normalizedPreferred;
    }
    return QDir::homePath();
}

QStringList tbcInputFilter()
{
    return {QStringLiteral("TBC files (*.tbc *.ytbc *.ctbc *.tbcy *.tbcc)"),
            QStringLiteral("Metadata (*.db *.json)"),
            QStringLiteral("All Files (*)")};
}
} // namespace

ProcessVbiDialog::ProcessVbiDialog(QWidget *parent) :
    QDialog(parent)
{
    buildUi();
    setBusy(false);
    appendStatus(tr("Ready."));
}

void ProcessVbiDialog::setSourceDirectory(const QString &directory)
{
    const QString normalizedDirectory = QDir::cleanPath(directory.trimmed());
    if (normalizedDirectory.isEmpty()) {
        return;
    }
    sourceDirectory = normalizedDirectory;
}

void ProcessVbiDialog::setDefaultInputTbc(const QString &tbcFilename)
{
    const QString normalizedInput = normalizePath(tbcFilename);
    if (normalizedInput.isEmpty()) {
        return;
    }
    const QFileInfo inputInfo(normalizedInput);
    if (inputInfo.exists() && inputInfo.isFile()) {
        inputTbcLineEdit->setText(inputInfo.absoluteFilePath());
        sourceDirectory = inputInfo.absolutePath();
        updateControlStates();
    } else {
        inputTbcLineEdit->setText(normalizedInput);
    }
}

void ProcessVbiDialog::setDefaultOutputMetadata(const QString &metadataFilename)
{
    const QString normalizedMetadata = normalizePath(metadataFilename);
    if (normalizedMetadata.isEmpty()) {
        return;
    }
    outputMetadataLineEdit->setText(normalizedMetadata);
}

void ProcessVbiDialog::buildUi()
{
    setWindowTitle(tr("Process VBI"));
    resize(760, 720);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // --- I/O group ---
    QGroupBox *ioGroupBox = new QGroupBox(tr("Input / output"), this);
    QGridLayout *ioLayout = new QGridLayout(ioGroupBox);
    ioLayout->setHorizontalSpacing(6);
    ioLayout->setVerticalSpacing(6);

    QLabel *inputLabel = new QLabel(tr("Input TBC"), ioGroupBox);
    ioLayout->addWidget(inputLabel, 0, 0, 1, 1);
    inputTbcLineEdit = new QLineEdit(ioGroupBox);
    inputTbcLineEdit->setPlaceholderText(tr("Source .tbc file to process"));
    ioLayout->addWidget(inputTbcLineEdit, 0, 1, 1, 2);
    inputTbcBrowseButton = new QPushButton(tr("Browse..."), ioGroupBox);
    ioLayout->addWidget(inputTbcBrowseButton, 0, 3, 1, 1);

    QLabel *outputLabel = new QLabel(tr("Output metadata"), ioGroupBox);
    ioLayout->addWidget(outputLabel, 1, 0, 1, 1);
    outputMetadataLineEdit = new QLineEdit(ioGroupBox);
    outputMetadataLineEdit->setPlaceholderText(tr("Optional: defaults to the input's auto-detected sidecar (.tbc.db/.json)"));
    ioLayout->addWidget(outputMetadataLineEdit, 1, 1, 1, 2);
    outputMetadataBrowseButton = new QPushButton(tr("Browse..."), ioGroupBox);
    ioLayout->addWidget(outputMetadataBrowseButton, 1, 3, 1, 1);

    mainLayout->addWidget(ioGroupBox);

    // --- Processing options group ---
    QGroupBox *optionsGroupBox = new QGroupBox(tr("Processing options"), this);
    QVBoxLayout *optionsLayout = new QVBoxLayout(optionsGroupBox);

    vbiCoreCheckBox = new QCheckBox(tr("VBI core (frame number / timecode / chapter / user code)"), optionsGroupBox);
    vbiCoreCheckBox->setToolTip(tr("Biphase-coded VBI on field lines 16-18 (PAL) / equivalent NTSC lines."));
    vbiCoreCheckBox->setChecked(true);
    optionsLayout->addWidget(vbiCoreCheckBox);

    ntscCheckBox = new QCheckBox(tr("NTSC-specific (FM code / white flag / video ID)"), optionsGroupBox);
    ntscCheckBox->setToolTip(tr("NTSC LaserDisc-specific data on field lines 10/11/20. Only applies to NTSC sources."));
    ntscCheckBox->setChecked(true);
    optionsLayout->addWidget(ntscCheckBox);

    vitcCheckBox = new QCheckBox(tr("VITC (vertical interval timecode)"), optionsGroupBox);
    vitcCheckBox->setToolTip(tr("SMPTE VITC timecode scanned across its candidate field lines."));
    vitcCheckBox->setChecked(true);
    optionsLayout->addWidget(vitcCheckBox);

    closedCaptionsCheckBox = new QCheckBox(tr("Closed captions (CEA-608)"), optionsGroupBox);
    closedCaptionsCheckBox->setToolTip(tr("Line 21 (525-line) / line 22 (625-line) closed caption data."));
    closedCaptionsCheckBox->setChecked(true);
    optionsLayout->addWidget(closedCaptionsCheckBox);

    teletextCheckBox = new QCheckBox(tr("Teletext (HTML export)"), optionsGroupBox);
    teletextCheckBox->setToolTip(tr("Decode teletext and write generated HTML pages. Off by default - most tapes have no teletext. Failure is non-fatal."));
    optionsLayout->addWidget(teletextCheckBox);

    vitsCheckBox = new QCheckBox(tr("VITS metrics (SNR)"), optionsGroupBox);
    vitsCheckBox->setToolTip(tr("Run the VITS analyser in-process to (re)compute white/black SNR metrics. Off by default. Failure is non-fatal."));
    optionsLayout->addWidget(vitsCheckBox);

    // Teletext advanced options - enabled only when teletext is ticked.
    teletextGroupBox = new QGroupBox(tr("Teletext options"), optionsGroupBox);
    QFormLayout *teletextForm = new QFormLayout(teletextGroupBox);

    QHBoxLayout *htmlDirRow = new QHBoxLayout();
    teletextHtmlDirLineEdit = new QLineEdit(teletextGroupBox);
    teletextHtmlDirLineEdit->setPlaceholderText(tr("Default: <input>_teletext_html beside the TBC"));
    QPushButton *teletextBrowseButton = new QPushButton(tr("Browse..."), teletextGroupBox);
    htmlDirRow->addWidget(teletextHtmlDirLineEdit);
    htmlDirRow->addWidget(teletextBrowseButton);
    teletextForm->addRow(tr("HTML output directory:"), htmlDirRow);

    teletextTapeFormatLineEdit = new QLineEdit(teletextGroupBox);
    teletextTapeFormatLineEdit->setText(QStringLiteral("vhs"));
    teletextForm->addRow(tr("Tape format profile:"), teletextTapeFormatLineEdit);

    teletextMinDuplicatesSpinBox = new QSpinBox(teletextGroupBox);
    teletextMinDuplicatesSpinBox->setRange(1, 999);
    teletextMinDuplicatesSpinBox->setValue(1);
    teletextForm->addRow(tr("Minimum duplicates (squash):"), teletextMinDuplicatesSpinBox);

    optionsLayout->addWidget(teletextGroupBox);
    mainLayout->addWidget(optionsGroupBox);

    // --- Status / progress / log ---
    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    mainLayout->addWidget(statusLabel);

    progressBar = new QProgressBar(this);
    progressBar->setRange(0, 1);
    progressBar->setValue(0);
    mainLayout->addWidget(progressBar);

    logTextEdit = new QPlainTextEdit(this);
    logTextEdit->setReadOnly(true);
    // The system's fixed-pitch font (a style hint on the default family does
    // not make it monospaced)
    logTextEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mainLayout->addWidget(logTextEdit);

    // Run is the default: Enter in a field runs, never a Browse button
    auto *buttonBox = new QDialogButtonBox(this);
    runButton = buttonBox->addButton(tr("Run"), QDialogButtonBox::AcceptRole);
    runButton->setDefault(true);
    stopButton = buttonBox->addButton(tr("Stop"), QDialogButtonBox::ActionRole);
    closeButton = buttonBox->addButton(QDialogButtonBox::Close);
    mainLayout->addWidget(buttonBox);
    for (QPushButton *browseButton : {inputTbcBrowseButton, outputMetadataBrowseButton, teletextBrowseButton}) {
        browseButton->setAutoDefault(false);
    }

    // --- Connections ---
    connect(inputTbcBrowseButton, &QPushButton::clicked, this, &ProcessVbiDialog::onBrowseInputTbcClicked);
    connect(outputMetadataBrowseButton, &QPushButton::clicked, this, &ProcessVbiDialog::onBrowseOutputMetadataClicked);
    connect(teletextBrowseButton, &QPushButton::clicked, this, &ProcessVbiDialog::onBrowseTeletextDirClicked);
    connect(teletextCheckBox, &QCheckBox::toggled, this, &ProcessVbiDialog::onTeletextToggled);
    connect(runButton, &QPushButton::clicked, this, &ProcessVbiDialog::onRunClicked);
    connect(stopButton, &QPushButton::clicked, this, &ProcessVbiDialog::onStopClicked);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto stateUpdater = [this]() { updateControlStates(); };
    connect(inputTbcLineEdit, &QLineEdit::textChanged, this, stateUpdater);
    connect(vitsCheckBox, &QCheckBox::toggled, this, stateUpdater);

    onTeletextToggled(teletextCheckBox->isChecked());
    updateControlStates();
}

void ProcessVbiDialog::setBusy(bool enabled)
{
    runInProgress = enabled;
    if (enabled) {
        cancelRequested = false;
    }

    inputTbcLineEdit->setEnabled(!enabled);
    inputTbcBrowseButton->setEnabled(!enabled);
    outputMetadataLineEdit->setEnabled(!enabled);
    outputMetadataBrowseButton->setEnabled(!enabled);
    vbiCoreCheckBox->setEnabled(!enabled);
    ntscCheckBox->setEnabled(!enabled);
    vitcCheckBox->setEnabled(!enabled);
    closedCaptionsCheckBox->setEnabled(!enabled);
    teletextCheckBox->setEnabled(!enabled);
    vitsCheckBox->setEnabled(!enabled);
    teletextGroupBox->setEnabled(!enabled && teletextCheckBox->isChecked());
    runButton->setEnabled(!enabled);
    closeButton->setEnabled(!enabled);
    stopButton->setEnabled(enabled);

    if (!enabled) {
        updateControlStates();
    }
}

void ProcessVbiDialog::updateControlStates()
{
    if (runInProgress) {
        return;
    }

    const QFileInfo inputInfo(normalizePath(inputTbcLineEdit->text()));
    const bool hasInput = !inputInfo.fileName().isEmpty();
    runButton->setEnabled(hasInput);

    teletextGroupBox->setEnabled(teletextCheckBox->isChecked());
}

QString ProcessVbiDialog::normalizePath(const QString &path) const
{
    QString normalized = path.trimmed();
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (normalized.isEmpty()) {
        return QString();
    }
    return QDir::cleanPath(normalized);
}

void ProcessVbiDialog::appendStatus(const QString &text)
{
    statusLabel->setText(text);
}

void ProcessVbiDialog::appendLog(const QString &text)
{
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    logTextEdit->appendPlainText(QStringLiteral("[%1] %2").arg(timestamp, text));
    QScrollBar *scrollBar = logTextEdit->verticalScrollBar();
    if (scrollBar) {
        scrollBar->setValue(scrollBar->maximum());
    }
}

QString ProcessVbiDialog::formatCommand(const QString &program, const QStringList &arguments) const
{
    QStringList commandParts;
    commandParts << quoteForShell(program);
    for (const QString &argument : arguments) {
        commandParts << quoteForShell(argument);
    }
    return commandParts.join(QLatin1Char(' '));
}

// The dialog runs its own binary in CLI mode, so every option exists
QStringList ProcessVbiDialog::buildToolArguments(const VbiProcessingOptions &opts) const
{
    QStringList args;

    if (!opts.vbiCore) {
        args << QStringLiteral("--no-vbi-core");
    }
    if (!opts.ntsc) {
        args << QStringLiteral("--no-ntsc");
    }
    if (!opts.vitc) {
        args << QStringLiteral("--no-vitc");
    }
    if (!opts.closedCaptions) {
        args << QStringLiteral("--no-closed-captions");
    }

    if (opts.teletext) {
        args << QStringLiteral("--teletext");
        if (!opts.teletextHtmlDir.isEmpty()) {
            args << QStringLiteral("--teletext-html-dir") << opts.teletextHtmlDir;
        }
        if (!opts.teletextTapeFormat.trimmed().isEmpty()
            && opts.teletextTapeFormat.trimmed().compare(QStringLiteral("vhs"), Qt::CaseInsensitive) != 0) {
            args << QStringLiteral("--teletext-tape-format") << opts.teletextTapeFormat.trimmed();
        }
        if (opts.teletextMinDuplicates > 1) {
            args << QStringLiteral("--teletext-min-duplicates") << QString::number(opts.teletextMinDuplicates);
        }
    }

    if (opts.vits) {
        args << QStringLiteral("--vits");
    }

    return args;
}

// Log each complete line of the run's output (\r-rewritten progress lines too)
void ProcessVbiDialog::consumeOutput(const QByteArray &chunk)
{
    QByteArray normalized = chunk;
    normalized.replace('\r', '\n');
    pendingOutputBuffer.append(normalized);

    qsizetype lineBreakIndex = -1;
    while ((lineBreakIndex = pendingOutputBuffer.indexOf('\n')) >= 0) {
        const QString lineText = QString::fromLocal8Bit(pendingOutputBuffer.left(lineBreakIndex)).trimmed();
        pendingOutputBuffer.remove(0, lineBreakIndex + 1);
        if (!lineText.isEmpty()) {
            lastOutputLine = lineText;
            appendLog(lineText);
        }
    }
}

// The run has ended (or never started, with startError set): report it
void ProcessVbiDialog::finishRun(const QString &startError)
{
    consumeOutput(process->readAllStandardOutput());
    const QString trailingLine = QString::fromLocal8Bit(pendingOutputBuffer).trimmed();
    if (!trailingLine.isEmpty()) {
        lastOutputLine = trailingLine;
        appendLog(trailingLine);
    }
    pendingOutputBuffer.clear();

    progressBar->setRange(0, 1);
    progressBar->setValue(1);

    QString errorMessage = startError;
    if (errorMessage.isEmpty()) {
        if (cancelRequested) {
            errorMessage = tr("Cancelled by user.");
        } else if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
            errorMessage = !lastOutputLine.isEmpty()
                               ? lastOutputLine
                               : tr("tbc-process-vbi failed with exit code %1.").arg(process->exitCode());
        }
    }
    process->deleteLater();
    process = nullptr;
    setBusy(false);

    if (!errorMessage.isEmpty()) {
        appendStatus(tr("Failed: %1").arg(errorMessage));
        if (!cancelRequested) {
            QMessageBox::warning(this, tr("Process failed"), errorMessage);
        }
    } else {
        appendLog(tr("tbc-process-vbi completed."));
        appendStatus(tr("VBI processing completed for %1").arg(runInputTbc));
    }
}

void ProcessVbiDialog::onBrowseInputTbcClicked()
{
    const QString startPath = !normalizePath(inputTbcLineEdit->text()).isEmpty()
        ? normalizePath(inputTbcLineEdit->text())
        : chooseStartDirectory(sourceDirectory);
    const QString path = QFileDialog::getOpenFileName(this, tr("Select input TBC file"), startPath, tbcInputFilter().join(QStringLiteral(";;")));
    if (!path.isEmpty()) {
        inputTbcLineEdit->setText(path);
        const QFileInfo info(path);
        if (info.exists()) {
            sourceDirectory = info.absolutePath();
        }
        updateControlStates();
    }
}

void ProcessVbiDialog::onBrowseOutputMetadataClicked()
{
    const QString startPath = !normalizePath(outputMetadataLineEdit->text()).isEmpty()
        ? normalizePath(outputMetadataLineEdit->text())
        : chooseStartDirectory(sourceDirectory);
    const QString path = QFileDialog::getSaveFileName(this, tr("Select output metadata file"), startPath, QStringLiteral("Metadata (*.db *.json);;All Files (*)"));
    if (!path.isEmpty()) {
        outputMetadataLineEdit->setText(path);
    }
}

void ProcessVbiDialog::onBrowseTeletextDirClicked()
{
    const QString startDir = normalizePath(teletextHtmlDirLineEdit->text());
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select teletext HTML output directory"),
                                                           startDir.isEmpty() ? QString() : startDir);
    if (!dir.isEmpty()) {
        teletextHtmlDirLineEdit->setText(dir);
    }
}

void ProcessVbiDialog::onTeletextToggled(bool checked)
{
    teletextGroupBox->setEnabled(checked);
}

void ProcessVbiDialog::onRunClicked()
{
    if (runInProgress) {
        return;
    }
    const QString inputTbc = QFileInfo(normalizePath(inputTbcLineEdit->text())).absoluteFilePath();
    if (inputTbc.isEmpty() || !QFileInfo::exists(inputTbc)) {
        QMessageBox::warning(this, tr("Input required"), tr("Please select a valid input TBC file."));
        return;
    }

    // Gather options from the UI.
    VbiProcessingOptions opts;
    opts.vbiCore = vbiCoreCheckBox->isChecked();
    opts.ntsc = ntscCheckBox->isChecked();
    opts.vitc = vitcCheckBox->isChecked();
    opts.closedCaptions = closedCaptionsCheckBox->isChecked();
    opts.teletext = teletextCheckBox->isChecked();
    opts.vits = vitsCheckBox->isChecked();
    opts.teletextHtmlDir = teletextHtmlDirLineEdit->text().trimmed();
    opts.teletextTapeFormat = teletextTapeFormatLineEdit->text().trimmed();
    opts.teletextMinDuplicates = teletextMinDuplicatesSpinBox->value();

    QStringList arguments;
    const QString outputMetadata = normalizePath(outputMetadataLineEdit->text());
    if (!outputMetadata.isEmpty()) {
        arguments << QStringLiteral("--output-metadata") << outputMetadata;
    }
    arguments << buildToolArguments(opts);
    arguments << inputTbc;

    // This same binary: arguments without --gui select its CLI mode
    const QString toolPath = QCoreApplication::applicationFilePath();

    setBusy(true);
    runInputTbc = inputTbc;
    pendingOutputBuffer.clear();
    lastOutputLine.clear();
    appendStatus(tr("Running tbc-process-vbi..."));
    appendLog(QStringLiteral("$ %1").arg(formatCommand(toolPath, arguments)));
    progressBar->setRange(0, 0); // indeterminate while the CLI runs

    process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::readyReadStandardOutput, this, [this]() {
        consumeOutput(process->readAllStandardOutput());
    });
    connect(process, &QProcess::finished, this, [this]() {
        finishRun(QString());
    });
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        // Every other error is followed by finished()
        if (error == QProcess::FailedToStart) {
            finishRun(tr("Unable to start tbc-process-vbi: %1").arg(process->errorString()));
        }
    });
    process->start(toolPath, arguments);
}

// Ask the run to stop; kill it if it hasn't after two seconds
void ProcessVbiDialog::onStopClicked()
{
    if (!process) {
        return;
    }
    cancelRequested = true;
    stopButton->setEnabled(false);
    appendStatus(tr("Stopping..."));
    process->terminate();
    QTimer::singleShot(2000, process, [stopping = process]() {
        if (stopping->state() != QProcess::NotRunning) {
            stopping->kill();
        }
    });
}

void ProcessVbiDialog::reject()
{
    if (runInProgress) {
        appendStatus(tr("Processing is running: Stop it before closing."));
        return;
    }
    QDialog::reject();
}
