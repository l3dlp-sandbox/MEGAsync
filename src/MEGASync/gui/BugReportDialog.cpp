#include "BugReportDialog.h"

#include "DialogOpener.h"
#include "TextDecorator.h"
#include "ui_BugReportDialog.h"
#include <BugReportController.h>
#include <MessageDialogOpener.h>
#include <TransfersModel.h>

#include <QCloseEvent>
#include <QRegExp>

using namespace mega;

BugReportDialog::BugReportDialog(QWidget* parent, MegaSyncLogger& logger):
    QDialog(parent),
    ui(new Ui::BugReportDialog),
    mController(std::make_unique<BugReportController>(logger))
{
    ui->setupUi(this);

    ui->lDescribeBug->setText(
        ui->lDescribeBug->text() +
        QString::fromUtf8("<span style=\"color:red; text-decoration:none;\">*</span>"));
    ui->bSubmit->setDefault(true);
    ui->bSubmit->setEnabled(false);

    mController->attachLogToReport(ui->cbAttachLogs->isChecked());

    connect(mController.get(),
            &BugReportController::reportFailed,
            this,
            &BugReportDialog::onReportFailed);

    connect(mController.get(),
            &BugReportController::reportUpdated,
            this,
            &BugReportDialog::onReportUpdated);

    connect(mController.get(),
            &BugReportController::reportUploadFinished,
            this,
            &BugReportDialog::onReportUploadedFinished);

    connect(mController.get(),
            &BugReportController::reportStarted,
            this,
            &BugReportDialog::onReportStarted);

    connect(mController.get(),
            &BugReportController::reportFinished,
            this,
            &BugReportDialog::onReportFinished);

    connect(ui->cbAttachLogs,
            &QCheckBox::toggled,
            this,
            [this](bool checked)
            {
                mController->attachLogToReport(checked);
            });

    connect(ui->teDescribeBug,
            &QTextEdit::textChanged,
            this,
            &BugReportDialog::onDescriptionChanged);
    connect(ui->leTitleBug, &QLineEdit::textChanged, this, &BugReportDialog::onTitleChanged);

    connect(ui->bCancel, &QPushButton::clicked, this, &BugReportDialog::onCancelClicked);
    connect(ui->bSubmit, &QPushButton::clicked, this, &BugReportDialog::onSubmitClicked);
    connect(ui->teDescribeBug,
            &QTextEdit::textChanged,
            this,
            &BugReportDialog::onDescribeBugTextChanged);
}

BugReportDialog::~BugReportDialog()
{
    delete ui;
}

void BugReportDialog::onReportStarted()
{
    openProgressDialog();

    // Open with min value
    onReportUpdated(0);
}

void BugReportDialog::onReportUploadedFinished()
{
    // We don´t allow cancelling the log submit to the SDK
    if (mProgressIndicatorDialog)
    {
        mProgressIndicatorDialog->setCancelButtonEnable(false);
    }
}

void BugReportDialog::onReportUpdated(int value)
{
    openProgressDialog();

    if (mProgressIndicatorDialog)
    {
        mProgressIndicatorDialog->setProgressBarValue(value);
    }
}

void BugReportDialog::closeProgressDialog()
{
    if (mProgressIndicatorDialog)
    {
        mProgressIndicatorDialog->close();
    }
}

void BugReportDialog::openProgressDialog()
{
    if (!mProgressIndicatorDialog)
    {
        mProgressIndicatorDialog = new ProgressIndicatorDialog(this);

        mProgressIndicatorDialog->setDialogDescription(
            tr("Bug report is uploading, it may take a few minutes"));
        mProgressIndicatorDialog->resetProgressBar();
        mProgressIndicatorDialog->setMinimumProgressBarValue(0);
        mProgressIndicatorDialog->setMaximumProgressBarValue(BugReportData::MAXIMUM_PERMIL_VALUE);
        mProgressIndicatorDialog->setProgressBarValue(0);

        connect(mProgressIndicatorDialog,
                &ProgressIndicatorDialog::cancelClicked,
                this,
                &BugReportDialog::cancelSendReport);

        DialogOpener::showDialog(mProgressIndicatorDialog);
    }
}

void BugReportDialog::setReportObject(const QString& title)
{
    ui->leTitleBug->setText(title);
}

void BugReportDialog::setReportText(const QString& desc)
{
    ui->teDescribeBug->setText(desc);
}

void BugReportDialog::onReportFinished()
{
    closeProgressDialog();

    MessageDialogInfo msgInfo;
    msgInfo.parent = this->parentWidget();
    msgInfo.titleText = tr("Bug report success!");
    msgInfo.descriptionText = tr("Your bug report has been submitted, a confirmation email "
                                 "will sent to you accordingly.");
    accept();
    MessageDialogOpener::information(msgInfo);
}

void BugReportDialog::onReportFailed()
{
    closeProgressDialog();

    MessageDialogInfo msgInfo;
    msgInfo.parent = this;
    msgInfo.titleText = tr("Error on submitting bug report");
    msgInfo.textFormat = Qt::RichText;
    msgInfo.buttons = QMessageBox::Ok;

    auto data(mController->getData());

    if (data.getStatus() == BugReportData::STATUS::LOG_UPLOAD_FAILED &&
        data.getTransferError() == MegaError::API_EEXIST)
    {
        msgInfo.descriptionText = tr("There is an ongoing report being uploaded.") +
                                  QString::fromUtf8("<br>") +
                                  tr("Please wait until the current upload is completed.");
        MessageDialogOpener::information(msgInfo);
    }
    else if (data.getStatus() == BugReportData::STATUS::REPORT_SUBMIT_FAILED &&
             data.getRequestError() == MegaError::API_ETOOMANY)
    {
        msgInfo.titleText = tr("You must wait 10 minutes before submitting another issue");
        Text::Link link(QString::fromLatin1("mailto:support@mega.nz"));
        QString text =
            tr("Please try again later or contact our support team via [A]support@mega.co.nz[/A] "
               "if the problem persists.");
        link.process(text);
        msgInfo.descriptionText = text;
        MessageDialogOpener::warning(msgInfo);
    }
    else
    {
        Text::Link link(QString::fromLatin1("mailto:support@mega.nz"));
        QString text =
            tr("Bug report can't be submitted due to some error. Please try again or contact our "
               "support team via [A]support@mega.co.nz[/A]");
        link.process(text);
        msgInfo.descriptionText = text;
        MessageDialogOpener::warning(msgInfo);
    }
}

void BugReportDialog::onSubmitClicked()
{
    mController->submitReport();
}

void BugReportDialog::onCancelClicked()
{
    reject();
}

void BugReportDialog::cancelSendReport()
{
    mController->prepareForCancellation();
    closeProgressDialog();

    MessageDialogInfo msgInfo;
    msgInfo.parent = this;
    msgInfo.titleText = tr("Are you sure you want to exit uploading?");
    msgInfo.descriptionText = tr("The bug report will not be submitted if you exit uploading.");

    QMap<QMessageBox::Button, QString> textsByButton;
    textsByButton.insert(QMessageBox::No, tr("Continue"));
    textsByButton.insert(QMessageBox::Yes, tr("Yes"));
    msgInfo.buttons = QMessageBox::Yes | QMessageBox::No;
    msgInfo.buttonsText = textsByButton;
    msgInfo.defaultButton = QMessageBox::Yes;

    msgInfo.finishFunc = [this](QPointer<MessageDialogResult> msg)
    {
        if (msg->result() == QMessageBox::Yes)
        {
            mController->cancel();
        }
        else
        {
            mController->resume();
        }
    };

    MessageDialogOpener::warning(msgInfo);
}

void BugReportDialog::onDescriptionChanged()
{
    auto description(ui->teDescribeBug->toPlainText());
    ui->bSubmit->setEnabled(!description.isEmpty());
    mController->setReportDescription(description);
}

void BugReportDialog::onTitleChanged()
{
    mController->setReportTitle(ui->leTitleBug->text());
}

void BugReportDialog::onDescribeBugTextChanged()
{
    if (ui->teDescribeBug->toPlainText().length() > mMaxDescriptionLength)
    {
        int diff = ui->teDescribeBug->toPlainText().length() -
                   mMaxDescriptionLength; // m_maxTextEditLength - just an integer
        QString newStr = ui->teDescribeBug->toPlainText();
        newStr.chop(diff);
        ui->teDescribeBug->setText(newStr);
        QTextCursor cursor(ui->teDescribeBug->textCursor());
        cursor.movePosition(QTextCursor::End, QTextCursor::MoveAnchor);
        ui->teDescribeBug->setTextCursor(cursor);
    }
}
