#ifndef BUGREPORTDIALOG_H
#define BUGREPORTDIALOG_H

#include <QDialog>
#include "megaapi.h"
#include "QTMegaTransferListener.h"
#include "MegaApplication.h"
#include <QProgressDialog>

namespace Ui {
class BugReportDialog;
}

class BugReportDialog : public QDialog, public mega::MegaTransferListener, public mega::MegaRequestListener
{
    Q_OBJECT

public:
    explicit BugReportDialog(QWidget *parent = 0);
    ~BugReportDialog();

    virtual void onTransferStart(mega::MegaApi *api, mega::MegaTransfer *transfer);
    virtual void onTransferUpdate(mega::MegaApi *api, mega::MegaTransfer *transfer);
    virtual void onTransferFinish(mega::MegaApi* api, mega::MegaTransfer *transfer, mega::MegaError* error);
    virtual void onTransferTemporaryError(mega::MegaApi *api, mega::MegaTransfer *transfer, mega::MegaError* e);

    virtual void onRequestFinish(mega::MegaApi* api, mega::MegaRequest *request, mega::MegaError* e);

private:
    Ui::BugReportDialog *ui;
    int currentTransfer;
    std::unique_ptr<QProgressDialog> sendProgress;

    long long totalBytes;
    long long transferredBytes;
    int lastpermil;

    bool warningShown;
    bool errorShown;
    bool preparing = false;
    QString reportFileName;

    const static int mMaxDescriptionLength = 3000;

protected:
    mega::MegaApi *megaApi;
    mega::QTMegaTransferListener *delegateTransferListener;
    mega::QTMegaRequestListener *delegateRequestListener;

    void showErrorMessage();
    void createSupportTicket();

private slots:
    void on_bSubmit_clicked();
    void on_bCancel_clicked();
    void cancelSendReport();
    void onDescriptionChanged();
    void onReadyForReporting();
    void on_teDescribeBug_textChanged();

};

#endif // BUGREPORTDIALOG_H