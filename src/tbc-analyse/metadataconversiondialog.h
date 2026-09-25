/******************************************************************************
 * metadataconversiondialog.h
 * tbc-analyse - TBC output analysis GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025 Simon Inns
 *
 * This file is part of tbc-tools.
 ******************************************************************************/

#ifndef METADATACONVERSIONDIALOG_H
#define METADATACONVERSIONDIALOG_H

#include <QDialog>

namespace Ui {
class MetadataConversionDialog;
}

class Configuration;

class MetadataConversionDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MetadataConversionDialog(QWidget *parent = nullptr);
    ~MetadataConversionDialog();

    void setSourceDirectory(const QString &directory);

    // Supplies the shared configuration so the metadata pickers open where
    // metadata was last converted. Not owned; may be left unset.
    void setConfiguration(Configuration *configuration);
    void setDefaultInput(const QString &inputFilename);

private slots:
    void on_inputBrowseButton_clicked();
    void on_outputBrowseButton_clicked();
    void on_convertButton_clicked();

private:
    void updateDirectionFromInput(bool forceOutputUpdate);
    Ui::MetadataConversionDialog *ui;
    Configuration *configuration = nullptr;  // shared settings; not owned, may be null
    QString sourceDirectory;
};

#endif // METADATACONVERSIONDIALOG_H
