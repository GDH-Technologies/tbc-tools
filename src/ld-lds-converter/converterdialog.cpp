/************************************************************************

    converterdialog.cpp

    ld-lds-converter - 10-bit .lds to FLAC/s16 converter for ld-decode
    Copyright (C) 2026 Simon Inns

    This file is part of tbc-tools.

    ld-lds-converter is free software: you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

************************************************************************/

#include "converterdialog.h"
#include "ui_converterdialog.h"

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QFile>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QMutexLocker>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSet>
#include <QTableWidgetItem>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>

namespace {
QString stripWrappingQuotes(const QString &value)
{
    QString strippedValue = value.trimmed();
    while (strippedValue.size() >= 2) {
        const QChar firstCharacter = strippedValue.front();
        const QChar lastCharacter = strippedValue.back();
        const bool wrappedInDoubleQuotes =
            firstCharacter == QLatin1Char('"') && lastCharacter == QLatin1Char('"');
        const bool wrappedInSingleQuotes =
            firstCharacter == QLatin1Char('\'') && lastCharacter == QLatin1Char('\'');
        if (!wrappedInDoubleQuotes && !wrappedInSingleQuotes) {
            break;
        }
        strippedValue = strippedValue.mid(1, strippedValue.size() - 2).trimmed();
    }
    return strippedValue;
}

void appendWhitespaceSeparatedCandidates(QStringList &candidateInputs, const QString &rawSegment)
{
    QString currentCandidate;
    for (const QChar character : rawSegment) {
        if (character.isSpace()) {
            const QString normalizedCandidate = stripWrappingQuotes(currentCandidate);
            if (!normalizedCandidate.isEmpty()) {
                candidateInputs << normalizedCandidate;
            }
            currentCandidate.clear();
            continue;
        }
        currentCandidate.append(character);
    }

    const QString normalizedCandidate = stripWrappingQuotes(currentCandidate);
    if (!normalizedCandidate.isEmpty()) {
        candidateInputs << normalizedCandidate;
    }
}

QStringList splitInputCandidates(const QString &rawInput)
{
    QStringList candidateInputs;
    const QString normalizedInput = stripWrappingQuotes(rawInput);
    if (normalizedInput.isEmpty()) {
        return candidateInputs;
    }

    const QString fileSchemePrefix = QStringLiteral("file://");
    if (!normalizedInput.contains(fileSchemePrefix, Qt::CaseInsensitive)) {
        candidateInputs << normalizedInput;
        return candidateInputs;
    }

    int scanPosition = 0;
    while (scanPosition < normalizedInput.size()) {
        const int fileSchemeStart =
            normalizedInput.indexOf(fileSchemePrefix, scanPosition, Qt::CaseInsensitive);
        if (fileSchemeStart < 0) {
            appendWhitespaceSeparatedCandidates(candidateInputs, normalizedInput.mid(scanPosition));
            break;
        }

        appendWhitespaceSeparatedCandidates(
            candidateInputs, normalizedInput.mid(scanPosition, fileSchemeStart - scanPosition));

        int fileSchemeEnd = normalizedInput.size();
        for (int characterIndex = fileSchemeStart; characterIndex < normalizedInput.size();
             characterIndex++) {
            if (normalizedInput.at(characterIndex).isSpace()) {
                fileSchemeEnd = characterIndex;
                break;
            }
        }

        const int nextEmbeddedScheme = normalizedInput.indexOf(
            fileSchemePrefix, fileSchemeStart + fileSchemePrefix.size(), Qt::CaseInsensitive);
        if (nextEmbeddedScheme >= 0 && nextEmbeddedScheme < fileSchemeEnd) {
            fileSchemeEnd = nextEmbeddedScheme;
        }

        const QString fileUriToken =
            stripWrappingQuotes(normalizedInput.mid(fileSchemeStart, fileSchemeEnd - fileSchemeStart));
        if (!fileUriToken.isEmpty()) {
            candidateInputs << fileUriToken;
        }
        scanPosition = fileSchemeEnd;
    }

    return candidateInputs;
}
QString normalizePathForCurrentPlatform(const QString &path)
{
    QString normalizedInput = path.trimmed();
    if (normalizedInput.isEmpty()) {
        return QString();
    }

    const QUrl inputUrl(normalizedInput);
    if (inputUrl.isValid() && inputUrl.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
        const QString localFile = inputUrl.toLocalFile();
        if (!localFile.isEmpty()) {
            normalizedInput = localFile;
        }
    }

    const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(normalizedInput));
    return QDir::toNativeSeparators(normalized);
}

} // namespace

ConverterDialog::ConverterDialog(QWidget *parent)
    : QDialog(parent),
      ui(new Ui::ConverterDialog),
      sourceDirectory(QDir::homePath()),
      userEditedOutput(false),
      conversionInProgress(false),
      cancelRequestedByUser(false),
      activeParallelConverters(),
      activeParallelConvertersMutex(),
      queuedInputFiles(),
      queuedInputRowIndex()
{
    ui->setupUi(this);
    // QDialog defaults to the Qt::Dialog window type, which most Linux window
    // managers decorate without a minimize button even when the hint is set.
    // Promote it to a plain top-level Qt::Window so the title bar gets the
    // standard minimize/maximize controls; exec() still keeps it modal.
    setWindowFlags(Qt::Window | Qt::WindowSystemMenuHint
                   | Qt::WindowMinimizeButtonHint | Qt::WindowMaximizeButtonHint
                   | Qt::WindowCloseButtonHint);
    setAcceptDrops(true);
    resetProgressDisplay();

    // No default button: Enter in the input field adds to the queue, and a
    // default (or auto-default) button would also be pressed by that Enter
    for (QPushButton *button : findChildren<QPushButton *>()) {
        button->setAutoDefault(false);
    }

    if (ui->outputFormatComboBox) {
        ui->outputFormatComboBox->clear();
        ui->outputFormatComboBox->addItem(tr("FLAC (default)"), static_cast<int>(DataConverter::OutputFormat::Flac));
        ui->outputFormatComboBox->addItem(tr("FLAC as .ldf (LaserDisc FLAC)"), static_cast<int>(DataConverter::OutputFormat::FlacLdf));
        ui->outputFormatComboBox->addItem(tr("s16 uncompressed"), static_cast<int>(DataConverter::OutputFormat::S16Raw));
        ui->outputFormatComboBox->setCurrentIndex(0);
    }
    if (ui->queuedInputsTableWidget) {
        ui->queuedInputsTableWidget->setColumnCount(2);
        ui->queuedInputsTableWidget->setHorizontalHeaderLabels(
            QStringList() << tr("Input file") << tr("Progress"));
        ui->queuedInputsTableWidget->verticalHeader()->setVisible(false);
        ui->queuedInputsTableWidget->horizontalHeader()->setStretchLastSection(false);
        ui->queuedInputsTableWidget->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        ui->queuedInputsTableWidget->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        connect(ui->queuedInputsTableWidget, &QTableWidget::itemSelectionChanged, this, [this]() {
            setConversionControlsEnabled(!conversionInProgress.load());
        });
    }

    if (ui->inputLineEdit) {
        // Typing into the manual entry field must never wipe the queue.
        // Only refresh the output preview when nothing is queued yet.
        connect(ui->inputLineEdit, &QLineEdit::textChanged, this, [this]() {
            setConversionControlsEnabled(!conversionInProgress.load());
            if (queuedInputFiles.isEmpty()) {
                updateOutputPathFromInput(false);
                resetProgressDisplay();
            }
        });
        // Pressing Enter appends the typed/pasted path(s) to the queue.
        connect(ui->inputLineEdit, &QLineEdit::returnPressed, this, [this]() {
            if (conversionInProgress.load()) {
                return;
            }
            const QString rawText = ui->inputLineEdit->text().trimmed();
            if (rawText.isEmpty()) {
                return;
            }
            const QStringList added = appendToInputQueue(QStringList() << rawText);
            if (added.isEmpty()) {
                if (ui->statusLabel) {
                    ui->statusLabel->setText(tr("Input already queued or invalid."));
                }
                return;
            }
            {
                const QSignalBlocker blocker(ui->inputLineEdit);
                ui->inputLineEdit->clear();
            }
            resetProgressDisplay();
            if (ui->statusLabel) {
                ui->statusLabel->setText(added.size() > 1
                    ? tr("Added %1 files to the queue (total queued: %2).").arg(added.size()).arg(queuedInputFiles.size())
                    : tr("Added 1 file to the queue (total queued: %1).").arg(queuedInputFiles.size()));
            }
        });
    }

    refreshQueuedInputDisplay();
}

ConverterDialog::~ConverterDialog()
{
    // reject() keeps the dialog open while converting, but if it is destroyed
    // anyway, the workers must not outlive what they report to
    if (runPending > 0) {
        cancelRequestedByUser = true;
        requestCancellationForActiveParallelConverters();
        QThreadPool::globalInstance()->waitForDone();
    }
    delete ui;
}

void ConverterDialog::setDefaultInput(const QString &inputFilename)
{
    setInputQueue(QStringList() << inputFilename);
}

void ConverterDialog::setDefaultInputs(const QStringList &inputFilenames)
{
    setInputQueue(inputFilenames);
}

void ConverterDialog::setDefaultOutput(const QString &outputFilename)
{
    const QString normalizedOutput = normalizePathForCurrentPlatform(outputFilename);
    if (normalizedOutput.isEmpty()) {
        return;
    }

    ui->outputLineEdit->setText(normalizedOutput);
    userEditedOutput = true;
}

void ConverterDialog::setDefaultFormat(DataConverter::OutputFormat outputFormat)
{
    if (!ui->outputFormatComboBox) {
        return;
    }

    const int comboIndex = ui->outputFormatComboBox->findData(static_cast<int>(outputFormat));
    if (comboIndex >= 0) {
        ui->outputFormatComboBox->setCurrentIndex(comboIndex);
    }
}

void ConverterDialog::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (mimeData == nullptr || !mimeData->hasUrls()) {
        event->ignore();
        return;
    }

    const QList<QUrl> urls = mimeData->urls();
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localFile = normalizePathForCurrentPlatform(url.toLocalFile());
        if (!localFile.isEmpty() && isLikelyLdsFile(localFile)) {
            event->acceptProposedAction();
            return;
        }
    }

    event->ignore();
}

void ConverterDialog::dropEvent(QDropEvent *event)
{
    if (conversionInProgress.load()) {
        event->ignore();
        return;
    }
    const QMimeData *mimeData = event->mimeData();
    if (mimeData == nullptr || !mimeData->hasUrls()) {
        event->ignore();
        return;
    }
    const QList<QUrl> urls = mimeData->urls();
    QStringList droppedInputFiles;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) {
            continue;
        }

        const QString localFile = normalizePathForCurrentPlatform(url.toLocalFile());
        if (!localFile.isEmpty() && isLikelyLdsFile(localFile)) {
            droppedInputFiles << localFile;
        }
    }

    if (!droppedInputFiles.isEmpty()) {
        const QStringList added = appendToInputQueue(droppedInputFiles);
        resetProgressDisplay();
        if (ui->statusLabel) {
            if (added.isEmpty()) {
                ui->statusLabel->setText(tr("No new files added (already in queue)."));
            } else if (added.size() > 1) {
                ui->statusLabel->setText(tr("Added %1 files to the queue (total queued: %2).").arg(added.size()).arg(queuedInputFiles.size()));
            } else {
                ui->statusLabel->setText(tr("Added 1 file to the queue (total queued: %1).").arg(queuedInputFiles.size()));
            }
        }
        event->acceptProposedAction();
        return;
    }

    if (ui->statusLabel) {
        ui->statusLabel->setText(tr("Drop a .lds file to auto-fill the input path."));
    }
    event->ignore();
}

void ConverterDialog::reject()
{
    if (conversionInProgress.load()) {
        closeWhenFinished = true;
        on_stopButton_clicked();
        return;
    }
    QDialog::reject();
}

void ConverterDialog::on_inputBrowseButton_clicked()
{
    if (conversionInProgress.load()) {
        return;
    }
    const QString filter = tr("LaserDisc samples (*.lds);;All Files (*)");
    const QStringList inputFileNames = QFileDialog::getOpenFileNames(this,
                                                                     tr("Add .lds input file(s) to queue"),
                                                                     sourceDirectory,
                                                                     filter);
    if (inputFileNames.isEmpty()) {
        return;
    }

    const QStringList added = appendToInputQueue(inputFileNames);
    resetProgressDisplay();
    if (ui->statusLabel) {
        if (added.isEmpty()) {
            ui->statusLabel->setText(tr("No new files added (already in queue)."));
        } else if (added.size() > 1) {
            ui->statusLabel->setText(tr("Added %1 files to the queue (total queued: %2).").arg(added.size()).arg(queuedInputFiles.size()));
        } else {
            ui->statusLabel->setText(tr("Added 1 file to the queue (total queued: %1).").arg(queuedInputFiles.size()));
        }
    }
}

void ConverterDialog::on_outputBrowseButton_clicked()
{
    const DataConverter::OutputFormat format = selectedOutputFormat();
    QString filter;
    if (format == DataConverter::OutputFormat::FlacLdf) {
        filter = tr("LaserDisc FLAC output (*.ldf);;All Files (*)");
    } else if (DataConverter::isFlacFamily(format)) {
        filter = tr("FLAC output (*.flac);;All Files (*)");
    } else {
        filter = tr("Signed 16-bit raw (*.s16 *.raw *.pcm);;All Files (*)");
    }

    QString suggestedOutput = normalizePathForCurrentPlatform(ui->outputLineEdit->text());
    if (suggestedOutput.isEmpty()) {
        QString suggestionBase;
        if (!queuedInputFiles.isEmpty()) {
            suggestionBase = queuedInputFiles.constFirst();
        } else {
            suggestionBase = normalizePathForCurrentPlatform(ui->inputLineEdit->text());
        }
        suggestedOutput = DataConverter::defaultOutputPath(suggestionBase, false, format);
    }

    const QString outputFileName = QFileDialog::getSaveFileName(this,
                                                                tr("Select output file"),
                                                                suggestedOutput,
                                                                filter);
    if (outputFileName.isEmpty()) {
        return;
    }

    userEditedOutput = true;
    ui->outputLineEdit->setText(normalizePathForCurrentPlatform(outputFileName));
    resetProgressDisplay();
    if (ui->statusLabel) {
        ui->statusLabel->clear();
    }

    const QFileInfo selectedInfo(ui->outputLineEdit->text());
    if (!selectedInfo.absolutePath().isEmpty()) {
        sourceDirectory = selectedInfo.absolutePath();
    }
}

void ConverterDialog::on_convertButton_clicked()
{
    QStringList inputFileNames = normalizedUniqueInputs(queuedInputFiles);
    if (inputFileNames.isEmpty()) {
        inputFileNames = normalizedUniqueInputs(QStringList() << ui->inputLineEdit->text());
    }

    const bool batchMode = inputFileNames.size() > 1;
    const QString requestedOutputPath = normalizePathForCurrentPlatform(ui->outputLineEdit->text());
    const DataConverter::OutputFormat format = selectedOutputFormat();
    const bool deleteAfterCompletion = ui->deleteAfterCompletionCheckBox != nullptr
                                       && ui->deleteAfterCompletionCheckBox->isChecked();
    const bool parallelCompressRequested = ui->parallelCompressCheckBox != nullptr
                                           && ui->parallelCompressCheckBox->isChecked();
    const bool useParallelCompression = parallelCompressRequested && batchMode;

    if (inputFileNames.isEmpty()) {
        ui->statusLabel->setText(tr("Please choose an input .lds file."));
        return;
    }

    QStringList missingInputFiles;
    for (const QString &inputFileName : inputFileNames) {
        if (!QFileInfo::exists(inputFileName)) {
            missingInputFiles << inputFileName;
        }
    }
    if (!missingInputFiles.isEmpty()) {
        ui->statusLabel->setText(tr("One or more input .lds files do not exist."));
        QMessageBox::warning(this,
                             tr("Error"),
                             tr("These input files do not exist:\n%1")
                                 .arg(missingInputFiles.join(QLatin1Char('\n'))));
        return;
    }

    QString outputDirectoryOverride;
    bool hasOutputDirectoryOverride = false;
    if (batchMode && !requestedOutputPath.isEmpty()) {
        const QFileInfo outputInfo(requestedOutputPath);
        const bool explicitDirectoryHint = requestedOutputPath.endsWith(QDir::separator())
                                           || requestedOutputPath.endsWith(QLatin1Char('/'))
                                           || requestedOutputPath.endsWith(QLatin1Char('\\'));
        if ((outputInfo.exists() && outputInfo.isDir()) || explicitDirectoryHint) {
            outputDirectoryOverride = requestedOutputPath;
            hasOutputDirectoryOverride = true;
        } else if (ui->statusLabel) {
            ui->statusLabel->setText(
                tr("Batch mode ignores single-file output override; using per-input output files."));
        }
    }

    QList<ConversionJob> conversionJobs;
    QList<ConversionResult> conversionResults;
    QSet<QString> reservedBatchOutputPaths;

    for (const QString &inputFileName : inputFileNames) {
        QString outputFileName;
        if (!batchMode) {
            outputFileName = requestedOutputPath;
            if (outputFileName.isEmpty()) {
                outputFileName = DataConverter::defaultOutputPath(inputFileName, false, format);
                if (!outputFileName.isEmpty()) {
                    ui->outputLineEdit->setText(outputFileName);
                }
            }
        } else if (hasOutputDirectoryOverride) {
            const QFileInfo inputInfo(inputFileName);
            outputFileName = QDir(outputDirectoryOverride).filePath(
                inputInfo.completeBaseName() + DataConverter::outputExtensionForFormat(format));
        } else {
            outputFileName = DataConverter::defaultOutputPath(inputFileName, false, format);
        }

        if (outputFileName.isEmpty()) {
            ConversionResult outputPathFailure;
            outputPathFailure.inputFileName = inputFileName;
            conversionResults << outputPathFailure;
            continue;
        }

        if (batchMode) {
            const QFileInfo outputInfo(outputFileName);
            const QString outputStem = outputInfo.completeBaseName();
            const QString outputSuffix = outputInfo.completeSuffix();
            const QString outputDirectory = outputInfo.absolutePath();
            QString uniqueOutputPath = outputFileName;
            int duplicateCounter = 2;
            while (reservedBatchOutputPaths.contains(uniqueOutputPath.toLower())) {
                const QString suffixToken = outputSuffix.isEmpty()
                                                ? QString()
                                                : QStringLiteral(".") + outputSuffix;
                uniqueOutputPath = QDir(outputDirectory).filePath(
                    QStringLiteral("%1_%2%3")
                        .arg(outputStem)
                        .arg(duplicateCounter)
                        .arg(suffixToken));
                duplicateCounter++;
            }
            outputFileName = uniqueOutputPath;
            reservedBatchOutputPaths.insert(outputFileName.toLower());
        }

        conversionJobs << ConversionJob{inputFileName, outputFileName};
    }

    if (conversionJobs.isEmpty()) {
        ui->statusLabel->setText(tr("No valid output paths could be prepared for conversion."));
        return;
    }
    conversionInProgress.store(true);

    setConversionControlsEnabled(false);
    cancelRequestedByUser = false;
    {
        QMutexLocker locker(&activeParallelConvertersMutex);
        activeParallelConverters.clear();
    }
    resetProgressDisplay();
    resetQueuedProgressDisplay();
    if (ui->stopButton) {
        ui->stopButton->setText(tr("Stop"));
        ui->stopButton->setEnabled(true);
    }

    if (parallelCompressRequested && !batchMode && ui->statusLabel) {
        ui->statusLabel->setText(
            tr("Parallel compress is available in batch mode; running sequential conversion."));
    }

    if (ui->progressBar) {
        ui->progressBar->setRange(0, std::max<int>(1, conversionJobs.size()));
        ui->progressBar->setValue(0);
    }
    if (ui->progressPercentLabel) {
        ui->progressPercentLabel->setText(tr("0%"));
    }

    // Every job (single, batch, parallel, or sequential) runs on a worker
    // thread and reports back through the event loop, so Stop is always
    // deliverable and the UI never freezes while a large file is processed.
    runJobs = conversionJobs;
    runJobResults = QList<std::optional<ConversionResult>>(conversionJobs.size());
    runResults = conversionResults;
    runLaunched = 0;
    runPending = 0;
    runCompleted = 0;
    runMaxParallel = useParallelCompression ? std::max(1, QThread::idealThreadCount()) : 1;
    runInputCount = inputFileNames.size();
    runBatchMode = batchMode;
    runDeleteAfterCompletion = deleteAfterCompletion;
    runFormat = format;
    launchPendingJobs();
}

// Start queued jobs until runMaxParallel are running. Not after Stop: any job
// not yet launched is simply skipped.
void ConverterDialog::launchPendingJobs()
{
    while (runLaunched < runJobs.size() && runPending < runMaxParallel && !cancelRequestedByUser.load()) {
        const ConversionJob job = runJobs.at(runLaunched);
        const int jobIndex = runLaunched++;
        setQueuedFileStatus(job.inputFileName, tr("Starting..."), 0);
        if (ui->statusLabel) {
            if (runBatchMode) {
                ui->statusLabel->setText(
                    tr("Converting %1 of %2: %3")
                        .arg(jobIndex + 1)
                        .arg(runJobs.size())
                        .arg(QFileInfo(job.inputFileName).fileName()));
            } else {
                ui->statusLabel->setText(tr("Converting..."));
            }
        }

        auto *watcher = new QFutureWatcher<ConversionResult>(this);
        connect(watcher, &QFutureWatcher<ConversionResult>::finished, this, [this, watcher, jobIndex]() {
            watcher->deleteLater();
            ConversionResult result;
            result.inputFileName = runJobs.at(jobIndex).inputFileName;
            try {
                result = watcher->result();
            } catch (...) {
                result.conversionOk = false;
                result.cancelled = cancelRequestedByUser.load();
            }
            runJobResults[jobIndex] = result;
            runPending--;
            runCompleted++;

            if (ui->progressBar) {
                ui->progressBar->setRange(0, std::max<int>(1, runJobs.size()));
                ui->progressBar->setValue(runCompleted);
            }
            if (ui->progressPercentLabel) {
                ui->progressPercentLabel->setText(tr("%1%").arg((runCompleted * 100) / runJobs.size()));
            }
            if (ui->statusLabel && runBatchMode) {
                ui->statusLabel->setText(
                    tr("Conversion progress: %1/%2 completed.").arg(runCompleted).arg(runJobs.size()));
            }

            launchPendingJobs();
            if (runPending == 0) {
                finishConversionRun();
            }
        });

        const DataConverter::OutputFormat format = runFormat;
        const bool batchMode = runBatchMode;
        const bool deleteAfterCompletion = runDeleteAfterCompletion;
        const bool verifyBeforeDelete = deleteAfterCompletion;
        watcher->setFuture(QtConcurrent::run(
            [this, job, format, batchMode, deleteAfterCompletion, verifyBeforeDelete]() -> ConversionResult {
                ConversionResult result;
                result.inputFileName = job.inputFileName;

                DataConverter converter(job.inputFileName,
                                        job.outputFileName,
                                        false,
                                        format,
                                        40000,
                                        8,
                                        verifyBeforeDelete);
                QObject::connect(&converter,
                                 &DataConverter::progressUpdated,
                                 this,
                                 [this, inputFileName = job.inputFileName, batchMode](qint64 processedBytes,
                                                                                      qint64 totalBytes) {
                                     updateQueuedFileProgress(inputFileName, processedBytes, totalBytes);
                                     if (!batchMode) {
                                         updateProgressDisplay(processedBytes, totalBytes);
                                     }
                                 },
                                 Qt::QueuedConnection);
                {
                    QMutexLocker locker(&activeParallelConvertersMutex);
                    activeParallelConverters.insert(&converter);
                }
                if (cancelRequestedByUser.load()) {
                    converter.requestCancel();
                }
                const bool conversionOk = converter.process();
                {
                    QMutexLocker locker(&activeParallelConvertersMutex);
                    activeParallelConverters.remove(&converter);
                }

                result.conversionOk = conversionOk;
                result.cancelled = converter.wasCancelled();
                if (result.conversionOk && deleteAfterCompletion) {
                    QFile inputFile(job.inputFileName);
                    if (!inputFile.remove()) {
                        result.deleteFailed = true;
                    }
                }
                return result;
            }));
        runPending++;
    }
}

// Every launched job has reported: re-enable the controls and report the run
void ConverterDialog::finishConversionRun()
{
    QList<ConversionResult> conversionResults = runResults;
    for (const std::optional<ConversionResult> &result : std::as_const(runJobResults)) {
        if (result) {
            conversionResults << *result;
        }
    }
    runJobs.clear();
    runJobResults.clear();
    runResults.clear();
    {
        QMutexLocker locker(&activeParallelConvertersMutex);
        activeParallelConverters.clear();
    }
    conversionInProgress.store(false);
    setConversionControlsEnabled(true);
    if (ui->stopButton) {
        ui->stopButton->setEnabled(false);
        ui->stopButton->setText(tr("Stop"));
    }

    int successfulConversions = 0;
    QStringList failedInputFiles;
    QStringList deleteFailedInputFiles;
    bool cancelled = cancelRequestedByUser.load();

    for (const ConversionResult &result : std::as_const(conversionResults)) {
        if (result.conversionOk) {
            successfulConversions++;
            if (result.deleteFailed) {
                deleteFailedInputFiles << result.inputFileName;
                setQueuedFileStatus(result.inputFileName, tr("Done (delete failed)"), 100);
            } else {
                setQueuedFileStatus(result.inputFileName, tr("Done"), 100);
            }
        } else if (result.cancelled) {
            cancelled = true;
            setQueuedFileStatus(result.inputFileName, tr("Cancelled"));
        } else {
            failedInputFiles << result.inputFileName;
            setQueuedFileStatus(result.inputFileName, tr("Failed"));
        }
    }

    const auto showDeleteFailureWarning = [this, &deleteFailedInputFiles]() {
        if (deleteFailedInputFiles.isEmpty()) {
            return;
        }
        QMessageBox::warning(this,
                             tr("Delete after completion"),
                             tr("Conversion completed, but these input files could not be deleted:\n%1")
                                 .arg(deleteFailedInputFiles.join(QLatin1Char('\n'))));
    };

    if (cancelled) {
        if (runBatchMode) {
            ui->statusLabel->setText(
                tr("Batch conversion cancelled (%1/%2 completed).")
                    .arg(successfulConversions)
                    .arg(runInputCount));
        } else {
            ui->statusLabel->setText(tr("Conversion cancelled."));
        }
        showDeleteFailureWarning();
        // Closing was asked for while the run was stopping
        if (closeWhenFinished) {
            closeWhenFinished = false;
            QDialog::reject();
        }
        return;
    }

    if (!failedInputFiles.isEmpty()) {
        if (runBatchMode) {
            ui->statusLabel->setText(
                tr("Batch conversion completed with errors (%1 succeeded, %2 failed).")
                    .arg(successfulConversions)
                    .arg(failedInputFiles.size()));
            QMessageBox::warning(this,
                                 tr("Batch conversion completed with errors"),
                                 tr("Failed to convert:\n%1")
                                     .arg(failedInputFiles.join(QLatin1Char('\n'))));
        } else {
            ui->statusLabel->setText(tr("Conversion failed."));
            QMessageBox::warning(this, tr("Error"), tr("Could not convert the selected file."));
        }
        showDeleteFailureWarning();
        return;
    }

    updateProgressDisplay(1, 1);
    if (runBatchMode) {
        ui->statusLabel->setText(tr("Batch conversion complete (%1 files).").arg(successfulConversions));
    } else {
        ui->statusLabel->setText(tr("Conversion complete."));
    }
    showDeleteFailureWarning();
}

void ConverterDialog::on_stopButton_clicked()
{
    if (!conversionInProgress.load()) {
        return;
    }

    cancelRequestedByUser = true;
    requestCancellationForActiveParallelConverters();
    if (ui->statusLabel) {
        ui->statusLabel->setText(tr("Stopping..."));
    }
    if (ui->stopButton) {
        ui->stopButton->setEnabled(false);
        ui->stopButton->setText(tr("Stopping..."));
    }
}

void ConverterDialog::on_outputFormatComboBox_currentIndexChanged(int index)
{
    Q_UNUSED(index)
    refreshOutputPreview(false);
    resetProgressDisplay();
}

void ConverterDialog::setInputQueue(const QStringList &inputFilenames)
{
    const QStringList normalizedInputs = normalizedUniqueInputs(inputFilenames);
    if (normalizedInputs.isEmpty()) {
        queuedInputFiles.clear();
        queuedInputRowIndex.clear();
        refreshQueuedInputDisplay();
        return;
    }

    queuedInputFiles = normalizedInputs;
    refreshQueuedInputDisplay();

    {
        const QSignalBlocker blocker(ui->inputLineEdit);
        ui->inputLineEdit->setText(normalizedInputs.constFirst());
        if (normalizedInputs.size() > 1) {
            ui->inputLineEdit->setToolTip(normalizedInputs.join(QLatin1Char('\n')));
        } else {
            ui->inputLineEdit->setToolTip(QString());
        }
    }

    userEditedOutput = false;
    refreshOutputPreview(true);

    const QFileInfo inputInfo(normalizedInputs.constFirst());
    if (inputInfo.exists()) {
        sourceDirectory = inputInfo.absolutePath();
    }
}

QStringList ConverterDialog::appendToInputQueue(const QStringList &inputFilenames)
{
    if (conversionInProgress.load()) {
        return QStringList();
    }

    const QStringList normalizedInputs = normalizedUniqueInputs(inputFilenames);
    if (normalizedInputs.isEmpty()) {
        return QStringList();
    }

    QStringList newlyAdded;
    for (const QString &normalizedInput : normalizedInputs) {
        bool alreadyPresent = false;
        for (const QString &existingInput : std::as_const(queuedInputFiles)) {
            if (existingInput.compare(normalizedInput, Qt::CaseInsensitive) == 0) {
                alreadyPresent = true;
                break;
            }
        }
        if (!alreadyPresent) {
            queuedInputFiles << normalizedInput;
            newlyAdded << normalizedInput;
        }
    }

    if (newlyAdded.isEmpty()) {
        return newlyAdded;
    }

    refreshQueuedInputDisplay();
    refreshOutputPreview(false);

    // Start the next Add.../browse near the most recently added file so that
    // adding files from different directories one-by-one stays convenient.
    const QFileInfo lastInfo(newlyAdded.last());
    if (lastInfo.exists()) {
        sourceDirectory = lastInfo.absolutePath();
    }

    return newlyAdded;
}

void ConverterDialog::refreshOutputPreview(bool forceUpdate)
{
    if (!queuedInputFiles.isEmpty()) {
        // In batch mode (>1) the per-input output paths are derived at convert
        // time; do not advertise a single-file output path here.
        if (queuedInputFiles.size() != 1) {
            return;
        }
        if (!forceUpdate && userEditedOutput && !ui->outputLineEdit->text().trimmed().isEmpty()) {
            return;
        }
        const QString normalizedInput = queuedInputFiles.constFirst();
        const QString suggestedOutput = DataConverter::defaultOutputPath(normalizedInput,
                                                                         false,
                                                                         selectedOutputFormat());
        if (suggestedOutput.isEmpty()) {
            return;
        }
        const QSignalBlocker blocker(ui->outputLineEdit);
        ui->outputLineEdit->setText(suggestedOutput);
        userEditedOutput = false;
        return;
    }

    // Queue empty: fall back to the manual entry field.
    updateOutputPathFromInput(forceUpdate);
}

void ConverterDialog::on_removeQueuedButton_clicked()
{
    if (conversionInProgress.load()) {
        return;
    }
    if (ui->queuedInputsTableWidget == nullptr || queuedInputFiles.isEmpty()) {
        return;
    }

    const QList<QTableWidgetItem *> selectedItems = ui->queuedInputsTableWidget->selectedItems();
    if (selectedItems.isEmpty()) {
        if (ui->statusLabel) {
            ui->statusLabel->setText(tr("Select one or more queued files to remove."));
        }
        return;
    }

    QSet<int> rowsToRemove;
    for (const QTableWidgetItem *item : selectedItems) {
        if (item != nullptr) {
            rowsToRemove.insert(item->row());
        }
    }

    QStringList survivingInputs;
    for (int row = 0; row < queuedInputFiles.size(); ++row) {
        if (!rowsToRemove.contains(row)) {
            survivingInputs << queuedInputFiles.at(row);
        }
    }

    queuedInputFiles = survivingInputs;
    refreshQueuedInputDisplay();
    refreshOutputPreview(false);
    resetProgressDisplay();
    if (ui->statusLabel) {
        ui->statusLabel->setText(tr("Removed %1 file(s) from the queue (remaining: %2).").arg(rowsToRemove.size()).arg(queuedInputFiles.size()));
    }
}

void ConverterDialog::on_clearQueuedButton_clicked()
{
    if (conversionInProgress.load()) {
        return;
    }
    if (queuedInputFiles.isEmpty()) {
        return;
    }

    queuedInputFiles.clear();
    queuedInputRowIndex.clear();
    refreshQueuedInputDisplay();
    refreshOutputPreview(true);
    resetProgressDisplay();
    if (ui->inputLineEdit) {
        const QSignalBlocker blocker(ui->inputLineEdit);
        ui->inputLineEdit->clear();
    }
    if (ui->statusLabel) {
        ui->statusLabel->setText(tr("Cleared the input queue."));
    }
}

void ConverterDialog::refreshQueuedInputDisplay()
{
    setConversionControlsEnabled(!conversionInProgress.load());
    if (ui->queuedInputsTableWidget == nullptr || ui->queuedSummaryLabel == nullptr) {
        return;
    }
    queuedInputRowIndex.clear();
    ui->queuedInputsTableWidget->clearContents();
    ui->queuedInputsTableWidget->setRowCount(queuedInputFiles.size());

    if (queuedInputFiles.isEmpty()) {
        ui->queuedSummaryLabel->setText(tr("Queued files: none"));
        return;
    }

    ui->queuedSummaryLabel->setText(tr("Queued files: %1").arg(queuedInputFiles.size()));
    for (int row = 0; row < queuedInputFiles.size(); row++) {
        const QString inputFileName = queuedInputFiles.at(row);
        const QString nativePath = QDir::toNativeSeparators(inputFileName);
        auto *pathItem = new QTableWidgetItem(nativePath);
        pathItem->setToolTip(nativePath);
        pathItem->setFlags(pathItem->flags() & ~Qt::ItemIsEditable);
        ui->queuedInputsTableWidget->setItem(row, 0, pathItem);

        auto *progressBar = new QProgressBar(ui->queuedInputsTableWidget);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setFormat(tr("Queued"));
        progressBar->setTextVisible(true);
        ui->queuedInputsTableWidget->setCellWidget(row, 1, progressBar);

        queuedInputRowIndex.insert(queuedFileKey(inputFileName), row);
    }
}

void ConverterDialog::resetQueuedProgressDisplay()
{
    for (const QString &inputFileName : std::as_const(queuedInputFiles)) {
        setQueuedFileStatus(inputFileName, tr("Queued"), 0);
    }
}

void ConverterDialog::updateQueuedFileProgress(const QString &inputFileName,
                                               qint64 processedBytes,
                                               qint64 totalBytes)
{
    if (ui->queuedInputsTableWidget == nullptr) {
        return;
    }

    const int row = queuedInputRowIndex.value(queuedFileKey(inputFileName), -1);
    if (row < 0) {
        return;
    }

    auto *progressBar =
        qobject_cast<QProgressBar *>(ui->queuedInputsTableWidget->cellWidget(row, 1));
    if (progressBar == nullptr) {
        return;
    }

    if (totalBytes <= 0) {
        progressBar->setRange(0, 0);
        progressBar->setFormat(tr("Working..."));
        return;
    }

    const qint64 clampedProcessed = std::clamp(processedBytes, static_cast<qint64>(0), totalBytes);
    const int percentage = static_cast<int>((clampedProcessed * 100) / totalBytes);
    progressBar->setRange(0, 100);
    progressBar->setValue(percentage);
    progressBar->setFormat(tr("%1%").arg(percentage));
}

void ConverterDialog::setQueuedFileStatus(const QString &inputFileName,
                                          const QString &statusText,
                                          int percentage)
{
    if (ui->queuedInputsTableWidget == nullptr) {
        return;
    }

    const int row = queuedInputRowIndex.value(queuedFileKey(inputFileName), -1);
    if (row < 0) {
        return;
    }

    auto *progressBar =
        qobject_cast<QProgressBar *>(ui->queuedInputsTableWidget->cellWidget(row, 1));
    if (progressBar == nullptr) {
        return;
    }

    if (percentage >= 0) {
        progressBar->setRange(0, 100);
        progressBar->setValue(std::clamp(percentage, 0, 100));
    } else if (progressBar->maximum() == 0) {
        progressBar->setRange(0, 100);
    }
    progressBar->setFormat(statusText);
}

QString ConverterDialog::queuedFileKey(const QString &inputFileName) const
{
    return QDir::cleanPath(QDir::fromNativeSeparators(inputFileName)).toLower();
}

void ConverterDialog::requestCancellationForActiveParallelConverters()
{
    QMutexLocker locker(&activeParallelConvertersMutex);
    for (DataConverter *converter : std::as_const(activeParallelConverters)) {
        if (converter != nullptr) {
            converter->requestCancel();
        }
    }
}

QStringList ConverterDialog::normalizedUniqueInputs(const QStringList &inputFilenames) const
{
    QStringList normalizedInputs;
    for (const QString &inputFilename : inputFilenames) {
        const QStringList expandedInputCandidates = splitInputCandidates(inputFilename);
        for (const QString &candidateInput : expandedInputCandidates) {
            const QString normalizedInput = normalizePathForCurrentPlatform(candidateInput);
            if (normalizedInput.isEmpty()) {
                continue;
            }

            bool alreadyIncluded = false;
            for (const QString &existingInput : normalizedInputs) {
                if (existingInput.compare(normalizedInput, Qt::CaseInsensitive) == 0) {
                    alreadyIncluded = true;
                    break;
                }
            }
            if (!alreadyIncluded) {
                normalizedInputs << normalizedInput;
            }
        }
    }

    return normalizedInputs;
}

void ConverterDialog::updateOutputPathFromInput(bool forceUpdate)
{
    const QStringList normalizedInputs = normalizedUniqueInputs(QStringList() << ui->inputLineEdit->text());
    if (normalizedInputs.isEmpty()) {
        return;
    }
    const QString normalizedInput = normalizedInputs.constFirst();

    if (!forceUpdate && userEditedOutput && !ui->outputLineEdit->text().trimmed().isEmpty()) {
        return;
    }

    const QString suggestedOutput = DataConverter::defaultOutputPath(normalizedInput,
                                                                     false,
                                                                     selectedOutputFormat());
    if (suggestedOutput.isEmpty()) {
        return;
    }

    const QSignalBlocker blocker(ui->outputLineEdit);
    ui->outputLineEdit->setText(suggestedOutput);
    userEditedOutput = false;
}

bool ConverterDialog::isLikelyLdsFile(const QString &filePath) const
{
    return QFileInfo(normalizePathForCurrentPlatform(filePath))
               .suffix()
               .compare(QStringLiteral("lds"), Qt::CaseInsensitive) == 0;
}

DataConverter::OutputFormat ConverterDialog::selectedOutputFormat() const
{
    const QVariant formatData = ui->outputFormatComboBox->currentData();
    if (!formatData.isValid()) {
        return DataConverter::OutputFormat::Flac;
    }
    return static_cast<DataConverter::OutputFormat>(formatData.toInt());
}

// enabled: not converting. Convert also needs an input, Remove a selected
// queued file and Clear a queue.
void ConverterDialog::setConversionControlsEnabled(bool enabled)
{
    if (ui->convertButton) {
        const bool hasInput = !queuedInputFiles.isEmpty()
                              || (ui->inputLineEdit && !ui->inputLineEdit->text().trimmed().isEmpty());
        ui->convertButton->setEnabled(enabled && hasInput);
    }
    if (ui->inputBrowseButton) {
        ui->inputBrowseButton->setEnabled(enabled);
    }
    if (ui->outputBrowseButton) {
        ui->outputBrowseButton->setEnabled(enabled);
    }
    if (ui->inputLineEdit) {
        ui->inputLineEdit->setEnabled(enabled);
    }
    if (ui->outputLineEdit) {
        ui->outputLineEdit->setEnabled(enabled);
    }
    if (ui->outputFormatComboBox) {
        ui->outputFormatComboBox->setEnabled(enabled);
    }
    if (ui->parallelCompressCheckBox) {
        ui->parallelCompressCheckBox->setEnabled(enabled);
    }
    if (ui->deleteAfterCompletionCheckBox) {
        ui->deleteAfterCompletionCheckBox->setEnabled(enabled);
    }
    if (ui->queuedInputsTableWidget) {
        ui->queuedInputsTableWidget->setEnabled(enabled);
    }
    if (ui->removeQueuedButton) {
        ui->removeQueuedButton->setEnabled(enabled && ui->queuedInputsTableWidget
                                           && !ui->queuedInputsTableWidget->selectedItems().isEmpty());
    }
    if (ui->clearQueuedButton) {
        ui->clearQueuedButton->setEnabled(enabled && !queuedInputFiles.isEmpty());
    }
}

void ConverterDialog::resetProgressDisplay()
{
    if (ui->progressBar) {
        ui->progressBar->setRange(0, 100);
        ui->progressBar->setValue(0);
    }
    if (ui->progressPercentLabel) {
        ui->progressPercentLabel->setText(tr("0%"));
    }
}

void ConverterDialog::updateProgressDisplay(qint64 processedBytes, qint64 totalBytes)
{
    if (ui->progressBar == nullptr || ui->progressPercentLabel == nullptr) {
        return;
    }

    if (totalBytes <= 0) {
        ui->progressBar->setRange(0, 0);
        ui->progressPercentLabel->setText(tr("--%"));
    } else {
        const qint64 clampedProcessed = std::clamp(processedBytes, static_cast<qint64>(0), totalBytes);
        const int percentage = static_cast<int>((clampedProcessed * 100) / totalBytes);
        ui->progressBar->setRange(0, 100);
        ui->progressBar->setValue(percentage);
        ui->progressPercentLabel->setText(tr("%1%").arg(percentage));
    }
}
