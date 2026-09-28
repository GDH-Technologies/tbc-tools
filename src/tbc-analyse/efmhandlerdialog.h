/******************************************************************************
 * efmhandlerdialog.h
 * tbc-analyse - Dedicated EFM/AC3 handling workflow GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef EFMHANDLERDIALOG_H
#define EFMHANDLERDIALOG_H

#include <QDialog>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>

class Configuration;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

class EfmHandlerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit EfmHandlerDialog(QWidget *parent = nullptr);
    ~EfmHandlerDialog() override = default;

    void setSourceDirectory(const QString &directory);

    // Supplies the shared configuration so the EFM pickers open where EFM files
    // were last used. Not owned; may be left unset.
    void setConfiguration(Configuration *configuration);
    void setDefaultEfmInput(const QString &efmFilename);
    void setDefaultAc3Input(const QString &ac3Filename);
    void setSuggestedOutputBase(const QString &outputBasePath);

signals:
    void exportTracksPrepared(const QStringList &trackFiles, const QStringList &trackNames);

protected:
    // Escape and the title-bar close go through here; refused while a run is
    // in progress (Stop first)
    void reject() override;

private slots:
    void onAddEfmInputClicked();
    void onRemoveEfmInputClicked();
    void onClearEfmInputsClicked();
    void onBrowseOutputBaseClicked();
    void onBrowseAudioOutputClicked();
    void onBrowseDataOutputClicked();
    void onBrowseAc3InputClicked();
    void onBrowseAc3OutputClicked();
    void onRunClicked();
    void onCancelClicked();

private:
    struct CommandStep {
        QString title;
        QString program;
        QStringList arguments;
    };

    void buildUi();
    void setBusy(bool enabled);
    void updateControlStates();
    QString normalizePath(const QString &path) const;
    QString defaultOutputBaseFromInputs() const;
    void refreshDerivedOutputPaths(bool forceUpdate);
    void appendStatus(const QString &text);
    void appendLog(const QString &text);
    QString formatCommand(const QString &program, const QStringList &arguments) const;
    QString resolveExternalExecutable(const QStringList &toolNames) const;
    // A run: prepareRun() checks the inputs and builds runSteps; the steps then
    // run one QProcess at a time on the event loop (startNextStep), and
    // finishRun() reports the outcome
    bool prepareRun(QString *errorMessage);
    void startNextStep();
    void finishRun(bool success, const QString &errorMessage);

    // Where an EFM picker should open, and how a chosen path is remembered.
    // Both fall back to the seeded source directory when no configuration is set.
    QString efmStartDirectory() const;
    void rememberEfmDirectory(const QString &chosenPath);

    Configuration *configuration = nullptr;  // shared settings; not owned, may be null
    QString sourceDirectory;
    bool runInProgress = false;
    bool cancelRequested = false;
    QList<CommandStep> runSteps;
    int runStepIndex = 0;
    QPointer<QProcess> runProcess;
    std::unique_ptr<QTemporaryDir> runTempDirectory;  // intermediate files of the run
    QStringList runAudioTracks;                       // decoded audio to offer for export
    QByteArray runOutputBuffer;                       // the step's unterminated output
    QString runLastOutputLine;
    bool userEditedOutputBase = false;
    bool userEditedAudioOutput = false;
    bool userEditedDataOutput = false;
    bool userEditedAc3Output = false;

    QGroupBox *efmGroupBox = nullptr;
    QListWidget *efmInputListWidget = nullptr;
    QPushButton *addEfmInputButton = nullptr;
    QPushButton *removeEfmInputButton = nullptr;
    QPushButton *clearEfmInputsButton = nullptr;
    QCheckBox *noTimecodesCheckBox = nullptr;
    QCheckBox *stackF2CheckBox = nullptr;
    QLineEdit *outputBaseLineEdit = nullptr;
    QPushButton *outputBaseBrowseButton = nullptr;
    QCheckBox *extractAudioCheckBox = nullptr;
    QLineEdit *audioOutputLineEdit = nullptr;
    QPushButton *audioOutputBrowseButton = nullptr;
    QCheckBox *extractDataCheckBox = nullptr;
    QLineEdit *dataOutputLineEdit = nullptr;
    QPushButton *dataOutputBrowseButton = nullptr;
    QCheckBox *outputBadSectorMapCheckBox = nullptr;

    QGroupBox *ac3GroupBox = nullptr;
    QCheckBox *decodeAc3CheckBox = nullptr;
    QLineEdit *ac3InputLineEdit = nullptr;
    QPushButton *ac3InputBrowseButton = nullptr;
    QLineEdit *ac3OutputLineEdit = nullptr;
    QPushButton *ac3OutputBrowseButton = nullptr;
    QComboBox *ac3FormatComboBox = nullptr;

    QCheckBox *loadDecodedAudioForExportCheckBox = nullptr;
    QLabel *statusLabel = nullptr;
    QProgressBar *progressBar = nullptr;
    QPlainTextEdit *logTextEdit = nullptr;
    QPushButton *runButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QPushButton *closeButton = nullptr;
};

#endif // EFMHANDLERDIALOG_H
