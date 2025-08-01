#include "SettingsDialog.h"

#include "AccountDetailsDialog.h"
#include "AccountDetailsManager.h"
#include "BandwidthSettings.h"
#include "BugReportDialog.h"
#include "ChangePassword.h"
#include "CommonMessages.h"
#include "CreateRemoveSyncsManager.h"
#include "DialogOpener.h"
#include "FullName.h"
#include "MegaApplication.h"
#include "NodeSelectorSpecializations.h"
#include "Platform.h"
#include "PowerOptions.h"
#include "ProxySettings.h"
#include "RemoveBackupDialog.h"
#include "StatsEventHandler.h"
#include "ThemeManager.h"
#include "ui_SettingsDialog.h"
#include "Utilities.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMenu>
#include <QMessageBox>
#include <QRect>
#include <QShortcut>
#include <QtConcurrent/QtConcurrent>
#include <QTranslator>
#include <QUrl>

#include <cassert>
#include <memory>

#ifdef Q_OS_WINDOWS
extern Q_CORE_EXPORT int qt_ntfs_permission_lookup;
#else
#include "PermissionsDialog.h"
#endif

using namespace mega;
using namespace std::chrono_literals;

static const QString SYNCS_TAB_MENU_LABEL_QSS =
    QString::fromLatin1("QLabel{ border-image: url(%1); }");
static constexpr int NUMBER_OF_CLICKS_TO_DEBUG{5};

long long calculateCacheSize()
{
    long long cacheSize = 0;
    auto model(SyncInfo::instance());
    for (auto syncType: SyncInfo::AllHandledSyncTypes)
    {
        for (int i = 0; i < model->getNumSyncedFolders(syncType); i++)
        {
            auto syncSetting = model->getSyncSetting(i, syncType);
            QString syncPath = syncSetting->getLocalFolder();
            if (!syncPath.isEmpty())
            {
                Utilities::getFolderSize(syncPath + QDir::separator() +
                                             QString::fromUtf8(MEGA_DEBRIS_FOLDER),
                                         &cacheSize);
            }
        }
    }
    return cacheSize;
}

long long calculateRemoteCacheSize(MegaApi* mMegaApi)
{
    MegaNode* n = mMegaApi->getNodeByPath("//bin/SyncDebris");
    long long size = mMegaApi->getSize(n);
    delete n;
    return size;
}

SettingsDialog::SettingsDialog(MegaApplication* app, bool proxyOnly, QWidget* parent):
    QDialog(parent),
    mUi(new Ui::SettingsDialog),
    mApp(app),
    mPreferences(Preferences::instance()),
    mModel(SyncInfo::instance()),
    mMegaApi(app->getMegaApi()),
    mLoadingSettings(0),
    mThreadPool(ThreadPoolSingleton::getInstance()),
    mCacheSize(-1),
    mRemoteCacheSize(-1),
    mDebugCounter(0),
    usersUpdateListener(std::make_unique<UsersUpdateListener>())
{
    mUi->setupUi(this);

    connect(usersUpdateListener.get(),
            &UsersUpdateListener::userEmailUpdated,
            this,
            &SettingsDialog::onUserEmailChanged);
    mMegaApi->addListener(usersUpdateListener.get());

    // override whatever indexes might be set in .ui files (frequently checked in by mistake)
    mUi->wStack->setCurrentWidget(mUi->pGeneral);
    mUi->wStackFooter->setCurrentWidget(mUi->wGeneralFooter);
    // Add Ctrl+index keyboard shortcut for Settings tabs
    setShortCutsForToolBarItems();

    connect(mUi->wStack,
            &QStackedWidget::currentChanged,
            mUi->wStack,
            [=](const int& newValue)
            {
                if (newValue < mUi->wStackFooter->count())
                {
                    mUi->wStackFooter->setCurrentIndex(newValue);
                    // Setting new index in the stack widget cause the focus to be set to footer
                    // button avoid it, setting to main wStack to ease tab navigation among
                    // different controls.
                    mUi->wStack->setFocus();
                }
            });

    mUi->bGeneral->setChecked(true); // override whatever might be set in .ui
    mUi->gCache->setTitle(mUi->gCache->title().arg(QString::fromUtf8(MEGA_DEBRIS_FOLDER)));

#ifdef Q_OS_LINUX
    mUi->bUpdate->hide();
    mUi->cAutoUpdate->hide();
#endif

    mUi->cFinderIcons->hide();
#ifdef Q_OS_WINDOWS
    typedef LONG MEGANTSTATUS;

    typedef struct _MEGAOSVERSIONINFOW
    {
        DWORD dwOSVersionInfoSize;
        DWORD dwMajorVersion;
        DWORD dwMinorVersion;
        DWORD dwBuildNumber;
        DWORD dwPlatformId;
        WCHAR szCSDVersion[128]; // Maintenance string for PSS usage
    } MEGARTL_OSVERSIONINFOW, *PMEGARTL_OSVERSIONINFOW;

    typedef MEGANTSTATUS(WINAPI * RtlGetVersionPtr)(PMEGARTL_OSVERSIONINFOW);
    MEGARTL_OSVERSIONINFOW version = {0};
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod)
    {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning( \
    disable: 4191) // 'type cast': unsafe conversion from 'FARPROC' to 'RtlGetVersionPtr'
#endif
        RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        if (RtlGetVersion)
        {
            RtlGetVersion(&version);
            if (version.dwMajorVersion >= 10)
            {
                mUi->cFinderIcons->show();
            }
        }
    }
#endif

    mUi->gThemeSelector->hide();

#ifdef Q_OS_MACOS
    this->setWindowTitle(tr("Settings"));
    mUi->cStartOnStartup->setText(tr("Launch at login"));
#endif

    setProxyOnly(proxyOnly);

    AccountDetailsManager::instance()->attachStorageObserver(*this);
    AccountDetailsManager::instance()->attachBandwidthObserver(*this);
    AccountDetailsManager::instance()->attachAccountObserver(*this);

    connect(mApp,
            &MegaApplication::shellNotificationsProcessed,
            this,
            &SettingsDialog::onShellNotificationsProcessed);
    setOverlayCheckboxEnabled(!mApp->isShellNotificationProcessingOngoing(),
                              mUi->cOverlayIcons->isChecked());
    connect(mUi->bBackup, &QPushButton::clicked, this, &SettingsDialog::on_bBackup_clicked);
    connect(mUi->bSyncs, &QPushButton::clicked, this, &SettingsDialog::on_bSyncs_clicked);

    // React to AppState changes
    connect(AppState::instance().get(),
            &AppState::appStateChanged,
            this,
            &SettingsDialog::onAppStateChanged);

    startRequestTaskbarPinningTimer();
}

SettingsDialog::~SettingsDialog()
{
    AccountDetailsManager::instance()->dettachStorageObserver(*this);
    AccountDetailsManager::instance()->dettachBandwidthObserver(*this);
    AccountDetailsManager::instance()->dettachAccountObserver(*this);

    mMegaApi->removeListener(usersUpdateListener.get());

    delete mUi;
}

void SettingsDialog::openSettingsTab(int tab)
{
    if (mProxyOnly) // do not switch tabs when in guest mode
        return;

    switch (tab)
    {
        case GENERAL_TAB:
            mUi->bGeneral->click();
            break;

        case ACCOUNT_TAB:
            mUi->bAccount->click();
            break;

        case SYNCS_TAB:
            mUi->bSyncs->click();
            break;

        case BACKUP_TAB:
            mUi->bBackup->click();
            break;

        case SECURITY_TAB:
            mUi->bSecurity->click();
            break;

        case FOLDERS_TAB:
            mUi->bFolders->click();
            break;

        case NETWORK_TAB:
            mUi->bNetwork->click();
            break;

        case NOTIFICATIONS_TAB:
            mUi->bNotifications->click();
            break;

        default:
            break;
    }
}

void SettingsDialog::setProxyOnly(bool proxyOnly)
{
    mProxyOnly = proxyOnly;

    mUi->bGeneral->setEnabled(!proxyOnly);
    mUi->bAccount->setEnabled(!proxyOnly);
    mUi->bSyncs->setEnabled(!proxyOnly);
    mUi->bBackup->setEnabled(!proxyOnly);
    mUi->bSecurity->setEnabled(!proxyOnly);
    mUi->bFolders->setEnabled(!proxyOnly);
    mUi->bNotifications->setEnabled(!proxyOnly);

    if (proxyOnly)
    {
        mUi->bNetwork->setEnabled(true);
        mUi->bNetwork->setChecked(true);
    }
    else
    {
        loadSettings();
    }
}

void SettingsDialog::onAppStateChanged(AppState::AppStates oldAppState,
                                       AppState::AppStates newAppState)
{
    setProxyOnly(newAppState != AppState::NOMINAL);
}

void SettingsDialog::showGuestMode()
{
    mUi->wStack->setCurrentWidget(mUi->pNetwork);
    QPointer<ProxySettings> proxySettingsDialog = new ProxySettings(mApp, this);
    proxySettingsDialog->setAttribute(Qt::WA_DeleteOnClose);
    DialogOpener::showDialog(proxySettingsDialog,
                             [proxySettingsDialog, this]()
                             {
                                 if (proxySettingsDialog->result() == QDialog::Accepted)
                                 {
                                     mApp->applyProxySettings();
                                     if (mProxyOnly)
                                         accept(); // close Settings in guest mode
                                 }
                                 else
                                 {
                                     if (mProxyOnly)
                                         reject(); // close Settings in guest mode
                                 }
                             });
}

void SettingsDialog::loadSettings()
{
    mLoadingSettings++;

    if (mPreferences->logged())
    {
        connect(&mCacheSizeWatcher,
                &QFutureWatcher<long long>::finished,
                this,
                &SettingsDialog::onLocalCacheSizeAvailable);
        QFuture<long long> futureCacheSize = QtConcurrent::run(calculateCacheSize);
        mCacheSizeWatcher.setFuture(futureCacheSize);

        connect(&mRemoteCacheSizeWatcher,
                &QFutureWatcher<long long>::finished,
                this,
                &SettingsDialog::onRemoteCacheSizeAvailable);
        QFuture<long long> futureRemoteCacheSize =
            QtConcurrent::run(calculateRemoteCacheSize, mMegaApi);
        mRemoteCacheSizeWatcher.setFuture(futureRemoteCacheSize);
    }

    // General
    mUi->cFileVersioning->setChecked(!mPreferences->fileVersioningDisabled());
    mUi->cbSleepMode->setChecked(mPreferences->awakeIfActiveEnabled());
    mUi->cOverlayIcons->setChecked(!mPreferences->overlayIconsDisabled());
    mUi->cCacheSchedulerEnabled->setChecked(mPreferences->cleanerDaysLimit());
    mUi->sCacheSchedulerDays->setEnabled(mPreferences->cleanerDaysLimit());
    mUi->sCacheSchedulerDays->setValue(mPreferences->cleanerDaysLimitValue());
    updateCacheSchedulerDaysLabel();

    if (!mPreferences->canUpdate(MegaApplication::applicationFilePath()))
    {
        mUi->bUpdate->setEnabled(false);
        mUi->cAutoUpdate->setEnabled(false);
        mUi->cAutoUpdate->setChecked(false);
    }
    else
    {
        mUi->bUpdate->setEnabled(true);
        mUi->cAutoUpdate->setEnabled(true);
        mUi->cAutoUpdate->setChecked(mPreferences->updateAutomatically());
    }

    // if checked: make sure both sources are true
    mUi->cStartOnStartup->setChecked(mPreferences->startOnStartup() &&
                                     Platform::getInstance()->isStartOnStartupActive());

    // Language
    mUi->cLanguage->clear();
    mLanguageCodes.clear();
    QString fullPrefix = Preferences::TRANSLATION_FOLDER + Preferences::TRANSLATION_PREFIX;
    QDirIterator it(Preferences::TRANSLATION_FOLDER);
    QStringList languages;
    int currentIndex = -1;
    QString currentLanguage = mPreferences->language();
    while (it.hasNext())
    {
        QString file = it.next();
        if (file.startsWith(fullPrefix))
        {
            int extensionIndex = file.lastIndexOf(QString::fromUtf8("."));
            if ((extensionIndex - fullPrefix.size()) <= 0)
            {
                continue;
            }

            QString languageCode = file.mid(fullPrefix.size(), extensionIndex - fullPrefix.size());
            QString languageString = Utilities::languageCodeToString(languageCode);
            if (!languageString.isEmpty())
            {
                int i = 0;
                while (i < languages.size() && (languageString > languages[i]))
                {
                    i++;
                }
                languages.insert(i, languageString);
                mLanguageCodes.insert(i, languageCode);
            }
        }
    }

    for (int i = mLanguageCodes.size() - 1; i >= 0; i--)
    {
        if (currentLanguage.startsWith(mLanguageCodes[i]))
        {
            currentIndex = i;
            break;
        }
    }

    if (currentIndex == -1)
    {
        currentIndex = mLanguageCodes.indexOf(QString::fromUtf8("en"));
    }

    mUi->cLanguage->addItems(languages);
    mUi->cLanguage->setCurrentIndex(currentIndex);

    mUi->cbTheme->clear();
    mUi->cbTheme->addItems(ThemeManager::instance()->themesAvailable());

    auto themeIndex = static_cast<int>(mPreferences->getThemeType());
    if (themeIndex < mUi->cbTheme->count())
    {
        mUi->cbTheme->setCurrentIndex(themeIndex);
    }

    //Account
    mUi->lEmail->setText(mPreferences->email());
    auto fullName(
        (mPreferences->firstName() + QStringLiteral(" ") + mPreferences->lastName()).trimmed());
    mUi->lName->setText(fullName);

    // Update name in case it changes
    auto FullNameRequest = UserAttributes::FullName::requestFullName();
    connect(FullNameRequest.get(),
            &UserAttributes::FullName::fullNameReady,
            this,
            [this](const QString& fullName)
            {
                mUi->lName->setText(fullName);
            });

    // Avatar
    mUi->wAvatar->setUserEmail();

    // account type and details
    SettingsDialog::updateAccountElements();
    SettingsDialog::updateStorageElements();
    SettingsDialog::updateBandwidthElements();

    updateUploadFolder();
    updateDownloadFolder();

#ifdef Q_OS_WINDOWS
    mUi->cFinderIcons->setChecked(!mPreferences->leftPaneIconsDisabled());
#endif

    updateNetworkTab();

    // Folders tab
    mUi->syncSettings->setParentDialog(this);
    mUi->backupSettings->setParentDialog(this);

    // Syncs and backups
    mUi->syncSettings->setToolBarItem(mUi->bSyncs);
    mUi->backupSettings->setToolBarItem(mUi->bBackup);

    mLoadingSettings--;
}

// General -----------------------------------------------------------------------------------------

void deleteCache()
{
    MegaSyncApp->cleanLocalCaches(true);
}

void deleteRemoteCache(MegaApi* mMegaApi)
{
    MegaNode* n = mMegaApi->getNodeByPath("//bin/SyncDebris");
    mMegaApi->remove(n);
    delete n;
}

void SettingsDialog::setUpdateAvailable(bool updateAvailable)
{
    if (updateAvailable)
    {
        mUi->bUpdate->setText(tr("Install Update"));
    }
    else
    {
        mUi->bUpdate->setText(tr("Check for Updates"));
    }
}

void SettingsDialog::storageChanged()
{
    onCacheSizeAvailable();
}

void SettingsDialog::onLocalCacheSizeAvailable()
{
    mCacheSize = mCacheSizeWatcher.result();
    onCacheSizeAvailable();
}

void SettingsDialog::onRemoteCacheSizeAvailable()
{
    mRemoteCacheSize = mRemoteCacheSizeWatcher.result();
    onCacheSizeAvailable();
}

void SettingsDialog::on_bHelp_clicked()
{
    QString helpUrl = Preferences::BASE_MEGA_HELP_URL + QString::fromUtf8("/installs-apps/desktop");
    Utilities::openUrl(QUrl(helpUrl));
}

void SettingsDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange)
    {
        mUi->retranslateUi(this);

#ifdef Q_OS_MACOS
        mUi->cStartOnStartup->setText(tr("Launch at login"));
        this->setWindowTitle(tr("Settings"));
#else
        mUi->gCache->setTitle(mUi->gCache->title().arg(QString::fromUtf8(MEGA_DEBRIS_FOLDER)));
#endif

        onCacheSizeAvailable();
        updateNetworkTab();
        updateStorageElements();
        updateBandwidthElements();
        updateAccountElements();

        updateUploadFolder();
        updateDownloadFolder();
    }

    QDialog::changeEvent(event);
}

void SettingsDialog::on_bGeneral_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_GENERAL_TAB_CLICKED);

    emit userActivity();

    if ((mUi->wStack->currentWidget() == mUi->pGeneral))
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pGeneral);
}

void SettingsDialog::on_bClearCache_clicked()
{
    QString syncs;
    for (auto syncSetting: mModel->getAllSyncSettings())
    {
        QFileInfo fi(syncSetting->getLocalFolder() + QDir::separator() +
                     QString::fromUtf8(MEGA_DEBRIS_FOLDER));
        if (fi.exists() && fi.isDir())
        {
            syncs += QString::fromUtf8("<br/><a href=\"local://#%1\">%2</a>")
                         .arg(fi.absoluteFilePath() + QDir::separator())
                         .arg(syncSetting->name());
        }
    }

    MessageDialogInfo msgInfo;
    msgInfo.parent = this;
    msgInfo.titleText = tr("Clear local backup");
    msgInfo.descriptionText =
        tr("Backups of the previous versions of your synced files in your computer"
           " will be permanently deleted. Please, check your backup folders to see"
           " if you need to rescue something before continuing:") +
        QString::fromUtf8("<br/>") + syncs + QString::fromUtf8("<br/><br/>") +
        tr("Do you want to delete your local backup now?");
    msgInfo.textFormat = Qt::RichText;
    msgInfo.buttons = QMessageBox::Yes | QMessageBox::No;
    msgInfo.defaultButton = QMessageBox::No;
    msgInfo.finishFunc = [this](QPointer<MessageDialogResult> msg)
    {
        if (msg->result() == QMessageBox::Yes)
        {
            QtConcurrent::run(deleteCache);
            mCacheSize = 0;
            onCacheSizeAvailable();
        }
    };

    MessageDialogOpener::warning(msgInfo);
}

void SettingsDialog::on_bClearRemoteCache_clicked()
{
    std::shared_ptr<MegaNode> syncDebris(mMegaApi->getNodeByPath("//bin/SyncDebris"));
    if (!syncDebris)
    {
        mRemoteCacheSize = 0;
        return;
    }

    std::unique_ptr<const char[]> base64Handle(syncDebris->getBase64Handle());

    MessageDialogInfo msgInfo;
    msgInfo.parent = this;
    msgInfo.titleText = tr("Clear remote backup");
    msgInfo.descriptionText =
        tr("Backups of the previous versions of your synced files in MEGA will be"
           " permanently deleted. Please, check your [A] folder in the Rubbish Bin"
           " of your MEGA account to see if you need to rescue something"
           " before continuing.")
            .replace(QString::fromUtf8("[A]"),
                     QString::fromUtf8("<a href=\"mega://#fm/%1\">SyncDebris</a>")
                         .arg(QString::fromUtf8(base64Handle.get()))) +
        QString::fromUtf8("<br/><br/>") + tr("Do you want to delete your remote backup now?");
    msgInfo.textFormat = Qt::RichText;
    msgInfo.buttons = QMessageBox::Yes | QMessageBox::No;
    msgInfo.defaultButton = QMessageBox::No;
    msgInfo.finishFunc = [this](QPointer<MessageDialogResult> msg)
    {
        if (msg->result() == QMessageBox::Yes)
        {
            QtConcurrent::run(deleteRemoteCache, mMegaApi);
            mRemoteCacheSize = 0;
            onCacheSizeAvailable();
        }
    };

    MessageDialogOpener::warning(msgInfo);
}

void SettingsDialog::on_bClearFileVersions_clicked()
{
    MessageDialogInfo msgInfo;
    msgInfo.parent = this;
    msgInfo.descriptionText = tr("You are about to permanently remove all file versions."
                                 " Would you like to proceed?");
    msgInfo.buttons = QMessageBox::Yes | QMessageBox::No;
    msgInfo.textFormat = Qt::RichText;
    msgInfo.defaultButton = QMessageBox::No;
    msgInfo.finishFunc = [this](QPointer<MessageDialogResult> msg)
    {
        if (msg->result() == QMessageBox::Yes)
        {
            mMegaApi->removeVersions(new MegaListenerFuncExecuter(
                true,
                [](MegaApi* api, MegaRequest* request, MegaError* e)
                {
                    Q_UNUSED(api)
                    Q_UNUSED(request)
                    if (e->getErrorCode() == MegaError::API_OK)
                    {
                        AccountDetailsManager::instance()->updateUserStats(
                            AccountDetailsManager::Flag::STORAGE,
                            true,
                            USERSTATS_REMOVEVERSIONS);
                    }
                }));
        }
    };

    MessageDialogOpener::warning(msgInfo);
}

void SettingsDialog::on_cCacheSchedulerEnabled_toggled()
{
    if (mLoadingSettings)
        return;
    bool isEnabled = mUi->cCacheSchedulerEnabled->isChecked();
    mUi->sCacheSchedulerDays->setEnabled(isEnabled);
    mPreferences->setCleanerDaysLimit(isEnabled);
    if (isEnabled)
    {
        mApp->cleanLocalCaches();
    }
}

void SettingsDialog::on_sCacheSchedulerDays_valueChanged(int i)
{
    if (mLoadingSettings)
        return;
    if (mUi->cCacheSchedulerEnabled->isChecked())
    {
        mPreferences->setCleanerDaysLimitValue(i);
        updateCacheSchedulerDaysLabel();
        mApp->cleanLocalCaches();
    }
}

void SettingsDialog::on_cAutoUpdate_toggled(bool checked)
{
    if (mLoadingSettings)
        return;
    if (mUi->cAutoUpdate->isEnabled() && (checked != mPreferences->updateAutomatically()))
    {
        mPreferences->setUpdateAutomatically(checked);
        if (checked)
        {
            on_bUpdate_clicked();
        }
    }
}

void SettingsDialog::on_cStartOnStartup_toggled(bool checked)
{
    if (mLoadingSettings)
        return;
    if (!Platform::getInstance()->startOnStartup(checked))
    {
        // in case of failure - make sure configuration keeps the right value
        // LOG_debug << "Failed to " << (checked ? "enable" : "disable") << " MEGASync on startup.";
        mPreferences->setStartOnStartup(!checked);
    }
    else
    {
        mPreferences->setStartOnStartup(checked);
    }
}

void SettingsDialog::on_cLanguage_currentIndexChanged(int index)
{
    if (mLoadingSettings)
        return;
    if (index < 0)
        return; // QComboBox can emit with index -1; do nothing in that case
    QString selectedLanguage = mLanguageCodes[index];
    if (mPreferences->language() != selectedLanguage)
    {
        mPreferences->setLanguage(selectedLanguage);
        mApp->changeLanguage(selectedLanguage);
        updateCacheSchedulerDaysLabel();
        QString currentLanguage = mApp->getCurrentLanguageCode();
        mThreadPool->push(
            [=]()
            {
                mMegaApi->setLanguage(currentLanguage.toUtf8().constData());
                mMegaApi->setLanguagePreference(currentLanguage.toUtf8().constData());
            });
    }
}

void SettingsDialog::on_cFileVersioning_toggled(bool checked)
{
    if (mLoadingSettings)
        return;

    if (checked)
    {
        // This is actually saved to Preferences after the MegaApi call succeeds;
        // Warning: the parameter is actually "disable".
        mMegaApi->setFileVersionsOption(false);
    }
    else
    {
        // Restore the check while the user has not answered the warning dialog
        mUi->cFileVersioning->blockSignals(true);
        mUi->cFileVersioning->setChecked(true);

        MessageDialogInfo msgInfo;
        msgInfo.descriptionText = tr("Disabling file versioning will prevent"
                                     " the creation and storage of new file versions."
                                     " Do you want to continue?");
        msgInfo.buttons = QMessageBox::Yes | QMessageBox::No;
        msgInfo.defaultButton = QMessageBox::No;
        msgInfo.parent = this;
        msgInfo.finishFunc = [this](QPointer<MessageDialogResult> msg)
        {
            // We want to make changes only if the answer is Yes.
            // If the user clicks No or closes the dialog with he X button, we don't want to disable
            // the feature.
            if (msg->result() == QMessageBox::Yes)
            {
                // This is actually saved to Preferences after the MegaApi call succeeds;
                // Warning: the parameter is actually "disable".
                mMegaApi->setFileVersionsOption(true);
                mUi->cFileVersioning->setChecked(false);
            }
            mUi->cFileVersioning->blockSignals(false);
        };

        MessageDialogOpener::warning(msgInfo);
    }
}

void SettingsDialog::on_cbSleepMode_toggled(bool checked)
{
    if (mLoadingSettings)
        return;

    // This is actually saved to Preferences before calling the keepAwake, as this method uses the
    // setting state;
    mPreferences->setAwakeIfActive(checked);

    PowerOptions options;
    auto result = options.keepAwake(checked);

    if (checked && !result)
    {
        MessageDialogInfo msgInfo;
        msgInfo.titleText = tr("Sleep mode can't be setup");
        msgInfo.descriptionText =
            tr("Your operating system doesn't allow its sleep setting to be overwritten.");
        msgInfo.buttons = QMessageBox::Ok;
        msgInfo.defaultButton = QMessageBox::Ok;
        msgInfo.parent = this;
        msgInfo.finishFunc = [this, checked](QPointer<MessageDialogResult> msg)
        {
            mUi->cbSleepMode->blockSignals(true);
            mUi->cbSleepMode->setChecked(!checked);
            mPreferences->setAwakeIfActive(!checked);
            mUi->cbSleepMode->blockSignals(false);
        };

        MessageDialogOpener::critical(msgInfo);
    }
}

void SettingsDialog::on_cOverlayIcons_toggled(bool checked)
{
    if (mLoadingSettings)
        return;

    mPreferences->disableOverlayIcons(!checked);

    const int configuredSyncCount = mModel->getNumSyncedFolders(SyncInfo::AllHandledSyncTypes);
    if (configuredSyncCount <= 0)
        return;

    setOverlayCheckboxEnabled(false, checked);

#ifdef Q_OS_MACOS
    Platform::getInstance()->notifyRestartSyncFolders();
#endif
    mApp->notifyChangeToAllFolders();
}

#ifdef Q_OS_WINDOWS
void SettingsDialog::on_cFinderIcons_toggled(bool checked)
{
    if (mLoadingSettings)
        return;

    if (checked)
    {
        for (auto syncSetting: mModel->getAllSyncSettings())
        {
            Platform::getInstance()->addSyncToLeftPane(syncSetting->getLocalFolder(),
                                                       syncSetting->name(),
                                                       syncSetting->getSyncID());
        }
    }
    else
    {
        Platform::getInstance()->removeAllSyncsFromLeftPane();
    }
    mPreferences->disableLeftPaneIcons(!checked);
}
#endif

void SettingsDialog::on_cbTheme_currentIndexChanged(int index)
{
    if (mLoadingSettings)
    {
        return;
    }

    ThemeManager::instance()->setTheme(static_cast<Preferences::ThemeType>(index));
}

void SettingsDialog::on_bUpdate_clicked()
{
    if (mUi->bUpdate->text() == tr("Check for Updates"))
    {
        mApp->checkForUpdates();
    }
    else
    {
        mApp->triggerInstallUpdate();
    }
}

void SettingsDialog::on_bSendBug_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_REPORT_ISSUE_CLICKED);

    QPointer<BugReportDialog> dialog = new BugReportDialog(this, mApp->getLogger());
    DialogOpener::showDialog(dialog);
}

void SettingsDialog::onCacheSizeAvailable()
{
    if (!mPreferences->logged())
        return;

    auto versionsStorage(mPreferences->versionsStorage());

    mUi->lFileVersionsSize->setText(Utilities::getSizeString(versionsStorage));

    if (mCacheSize != -1)
    {
        mUi->lCacheSize->setText(Utilities::getSizeString(mCacheSize));
    }

    if (mRemoteCacheSize != -1)
    {
        mUi->lRemoteCacheSize->setText(Utilities::getSizeString(mRemoteCacheSize));
    }

    mUi->bClearCache->setEnabled(mCacheSize > 0);
    mUi->bClearRemoteCache->setEnabled(mRemoteCacheSize > 0);
    mUi->bClearFileVersions->setEnabled(versionsStorage > 0);
}

// Account -----------------------------------------------------------------------------------------
void SettingsDialog::updateStorageElements()
{
    auto totalStorage = mPreferences->totalStorage();
    auto usedStorage = mPreferences->usedStorage();
    if (totalStorage == 0)
    {
        mUi->pStorageQuota->setValue(0);
        mUi->lStorage->setText(tr("Data temporarily unavailable"));
        mUi->bStorageDetails->setEnabled(false);
    }
    else
    {
        mUi->bStorageDetails->setEnabled(true);

        if (Utilities::isBusinessAccount())
        {
            mUi->lStorage->setText(Utilities::createSimpleUsedString(usedStorage));
        }
        else
        {
            int percentage = Utilities::partPer(usedStorage, totalStorage);

            setProgressState(QLatin1String("storageState"));

            mUi->pStorageQuota->setValue(std::min(percentage, mUi->pStorageQuota->maximum()));
            mUi->lStorage->setText(
                Utilities::createCompleteUsedString(usedStorage, totalStorage, percentage));
        }
    }
}

void SettingsDialog::updateBandwidthElements()
{
    int accountType = mPreferences->accountType();
    auto totalBandwidth = mPreferences->totalBandwidth();
    auto usedBandwidth = mPreferences->usedBandwidth();
    mUi->lBandwidthFree->hide();

    if (accountType == Preferences::ACCOUNT_TYPE_FREE)
    {
        mUi->lBandwidth->setText(
            tr("Used quota for the last %n hour:", "", mPreferences->bandwidthInterval()));
        mUi->lBandwidthFree->show();
        mUi->lBandwidthFree->setText(Utilities::getSizeString(usedBandwidth));
    }
    else if (Utilities::isBusinessAccount())
    {
        mUi->lBandwidth->setText(Utilities::createSimpleUsedString(usedBandwidth));
    }
    else
    {
        if (totalBandwidth == 0)
        {
            mUi->pTransferQuota->setValue(0);
            mUi->lBandwidth->setText(tr("Data temporarily unavailable"));
        }
        else
        {
            int percentage = Utilities::partPer(usedBandwidth, totalBandwidth);

            setProgressState(QLatin1String("transferState"));

            mUi->pTransferQuota->setValue(std::min(percentage, 100));
            mUi->lBandwidth->setText(
                Utilities::createCompleteUsedString(usedBandwidth,
                                                    totalBandwidth,
                                                    std::min(percentage, 100)));
        }
    }
}

void SettingsDialog::setProgressState(const QString& stateName)
{
    switch (mPreferences->getStorageState())
    {
        case MegaApi::STORAGE_STATE_PAYWALL:
        // Fallthrough
        case MegaApi::STORAGE_STATE_RED:
        {
            setProperty(stateName.toStdString().c_str(), QLatin1String("full"));
            break;
        }
        case MegaApi::STORAGE_STATE_ORANGE:
        {
            setProperty(stateName.toStdString().c_str(), QLatin1String("warning"));
            break;
        }
        case MegaApi::STORAGE_STATE_UNKNOWN:
        // Fallthrough
        case MegaApi::STORAGE_STATE_GREEN:
        // Fallthrough
        default:
        {
            setProperty(stateName.toStdString().c_str(), QLatin1String("ok"));
            break;
        }
    }
}

void SettingsDialog::updateAccountElements()
{
    QIcon icon;
    mUi->lAccountType->setText(Utilities::getReadablePlanFromId(mPreferences->accountType()));

    const QuotaState quotaState = MegaSyncApp->getTransferQuota()->quotaState();
    const bool isTransferOverquota = (quotaState != QuotaState::OK);
    mUi->bUpgrade->setVisible(Utilities::shouldDisplayUpgradeButton(isTransferOverquota));

    switch (mPreferences->accountType())
    {
        case Preferences::ACCOUNT_TYPE_FREE:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Free.png"));
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->hide();
            break;
        case Preferences::ACCOUNT_TYPE_PROI:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Pro_I.png"));
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        case Preferences::ACCOUNT_TYPE_PROII:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Pro_II.png"));
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        case Preferences::ACCOUNT_TYPE_PROIII:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Pro_III.png"));
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        case Preferences::ACCOUNT_TYPE_LITE:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Lite.png"));
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        case Preferences::ACCOUNT_TYPE_BUSINESS:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Business.png"));
            mUi->pStorageQuota->hide();
            mUi->pTransferQuota->hide();
            break;
        case Preferences::ACCOUNT_TYPE_PRO_FLEXI:
            icon = Utilities::getCachedPixmap(QString::fromLatin1(":/images/Small_Pro_Flexi.png"));
            mUi->pStorageQuota->hide();
            mUi->pTransferQuota->hide();
            break;
        case Preferences::ACCOUNT_TYPE_STARTER:
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        case Preferences::ACCOUNT_TYPE_BASIC:
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        case Preferences::ACCOUNT_TYPE_ESSENTIAL:
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
        default:
            icon = Utilities::getCachedPixmap(QString::fromUtf8(":/images/Small_Pro_I.png"));
            mUi->lAccountType->setText(QString());
            mUi->pStorageQuota->show();
            mUi->pTransferQuota->show();
            break;
    }

    mUi->lAccountType->setIcon(icon);
}

void SettingsDialog::on_bAccount_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_ACCOUNT_TAB_CLICKED);

    emit userActivity();

    if ((mUi->wStack->currentWidget() == mUi->pAccount))
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pAccount);
}

void SettingsDialog::on_lAccountType_clicked()
{
    mDebugCounter++;
    if (mDebugCounter == NUMBER_OF_CLICKS_TO_DEBUG)
    {
        mApp->toggleLogging();
        mDebugCounter = 0;
    }
}

void SettingsDialog::on_bUpgrade_clicked()
{
    Utilities::upgradeClicked();
}

void SettingsDialog::on_bMyAccount_clicked()
{
    Utilities::openUrl(QUrl(QString::fromUtf8("mega://#fm/account")));
}

void SettingsDialog::on_bStorageDetails_clicked()
{
    auto accountDetailsDialog = new AccountDetailsDialog();
    DialogOpener::showNonModalDialog<AccountDetailsDialog>(accountDetailsDialog);
}

void SettingsDialog::on_bLogout_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::LOGOUT_CLICKED);
    QString text;
    bool haveSyncs(false);
    bool haveBackups(false);

    // Check if we have syncs and backups
    if (mModel)
    {
        haveSyncs = !mModel->getSyncSettingsByType(mega::MegaSync::TYPE_TWOWAY).isEmpty();
        haveBackups = !mModel->getSyncSettingsByType(mega::MegaSync::TYPE_BACKUP).isEmpty();
    }

    // Set text according to situation
    if (haveSyncs && haveBackups)
    {
        text = tr("Synchronizations and backups will stop working.");
    }
    else if (haveBackups)
    {
        text = tr("Backups will stop working.");
    }
    else if (haveSyncs)
    {
        text = tr("Synchronizations will stop working.");
    }

    if (text.isEmpty())
    {
        mApp->unlink();
    }
    else
    {
        MessageDialogInfo msgInfo;
        msgInfo.titleText = tr("Log out");
        msgInfo.descriptionText = text + QLatin1Char(' ') + tr("Are you sure?");
        msgInfo.buttons = QMessageBox::Yes | QMessageBox::No;
        msgInfo.defaultButton = QMessageBox::Yes;
        msgInfo.parent = this;
        msgInfo.finishFunc = [](QPointer<MessageDialogResult> msg)
        {
            if (msg->result() == QMessageBox::Yes)
            {
                Utilities::queueFunctionInAppThread(
                    []()
                    {
                        MegaSyncApp->unlink();
                    });
            }
        };
        MessageDialogOpener::question(msgInfo);
    }
}

// Syncs -------------------------------------------------------------------------------------------
void SettingsDialog::setEnabledAllControls(const bool enabled)
{
    setGeneralTabEnabled(enabled);
    mUi->pAccount->setEnabled(enabled);
    mUi->pSecurity->setEnabled(enabled);
    mUi->pFolders->setEnabled(enabled);
    mUi->pNetwork->setEnabled(enabled);
    mUi->pNotifications->setEnabled(enabled);

    mUi->wStackFooter->setEnabled(enabled);
}

void SettingsDialog::setSyncAddButtonEnabled(const bool enabled, SettingsDialog::Tabs tab)
{
    SyncSettingsUIBase* syncSettings = nullptr;

    switch (tab)
    {
        case SYNCS_TAB:
            syncSettings = mUi->syncSettings;
            break;
        case BACKUP_TAB:
            syncSettings = mUi->backupSettings;
            break;
        default:
            MegaApi::log(MegaApi::LOG_LEVEL_WARNING,
                         QString::fromUtf8("Unexpected tab when setting add button enabled state")
                             .toUtf8()
                             .constData());
            break;
    }

    if (syncSettings != nullptr)
    {
        syncSettings->setAddButtonEnabled(enabled);
    }
}

void SettingsDialog::setGeneralTabEnabled(const bool enabled)
{
    // We want to keep only the "Send bug report" button enabled.
    // If we call setEnable() on the whole SettingsDialog, it will be
    // disabled and can't be enabled without enabling everything.
    // Another approach is to loop through all child widgets of SettingsDialog,
    // but we need to take care to skip all parents of BugReport button.
    // Experimentally it didn't work, so the last solution is to
    // call setEnable() manually for all controls and leave
    // Bug report controls as they are.

    mUi->gGeneral->setEnabled(enabled);
    mUi->gLanguage->setEnabled(enabled);
    mUi->gCache->setEnabled(enabled);
    mUi->gRemoteCache->setEnabled(enabled);
    mUi->gFileVersions->setEnabled(enabled);

#ifdef Q_OS_LINUX
    mUi->gSleepSettings->setEnabled(enabled);
#endif
}

void SettingsDialog::setOverlayCheckboxEnabled(const bool enabled, const bool checked)
{
    const int emptyIndex = 0;
    const int processingIndex = 1;
    mUi->cOverlayIcons->setEnabled(enabled);
    mUi->sOverlayMessageWidget->setCurrentIndex(enabled ? emptyIndex : processingIndex);
    if (enabled)
    {
        mUi->wWaitingSpinner->stop();
    }
    else
    {
        QString message =
            checked ? tr("Enabling sync status icons") : tr("Disabling sync status icons");
        mUi->lOverlayProcessing->setText(message);
        mUi->wWaitingSpinner->start();
    }
}

// Backup ----------------------------------------------------------------------------------------
void SettingsDialog::on_bBackup_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_BACKUP_TAB_CLICKED);

    emit userActivity();

    if (mUi->wStack->currentWidget() == mUi->pBackup)
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pBackup);

    SyncInfo::instance()->dismissUnattendedDisabledSyncs(MegaSync::TYPE_BACKUP);
}

void SettingsDialog::on_bBackupCenter_clicked()
{
    Utilities::openBackupCenter();
}

void SettingsDialog::on_bSyncs_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_SYNC_TAB_CLICKED);

    emit userActivity();

    if (mUi->wStack->currentWidget() == mUi->pSyncs)
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pSyncs);

    SyncInfo::instance()->dismissUnattendedDisabledSyncs(MegaSync::TYPE_TWOWAY);
}

// Security ----------------------------------------------------------------------------------------
void SettingsDialog::on_bSecurity_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_SECURITY_TAB_CLICKED);

    emit userActivity();

    if (mUi->wStack->currentWidget() == mUi->pSecurity)
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pSecurity);
}

void SettingsDialog::on_bExportMasterKey_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_EXPORT_KEY_CLICKED);

    QString defaultPath = QDir::toNativeSeparators(Utilities::getDefaultBasePath());
#ifndef _WIN32
    if (defaultPath.isEmpty())
    {
        defaultPath = QString::fromUtf8("/");
    }
#endif

    QDir dir(defaultPath);

    QFileDialog* dialog = new QFileDialog(this);
    dialog->setFileMode(QFileDialog::AnyFile);
    dialog->setOptions(QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    dialog->selectFile(dir.filePath(tr("MEGA-RECOVERYKEY")));
    dialog->setWindowTitle(tr("Export Master key"));
    dialog->setNameFilter(QString::fromUtf8("Txt file (*.txt)"));
    const QStringList schemes = QStringList(QStringLiteral("file"));
    dialog->setSupportedSchemes(schemes);
    dialog->setAcceptMode(QFileDialog::AcceptSave);
    DialogOpener::showDialog<QFileDialog>(
        dialog,
        [this, dialog]
        {
            if (dialog->result() == QDialog::Accepted)
            {
                auto fileNames = dialog->selectedFiles();

                if (fileNames.isEmpty())
                {
                    return;
                }

                QFile file(fileNames.first());
                if (!file.open(QIODevice::WriteOnly | QFile::Truncate))
                {
                    MessageDialogInfo msgInfo;
                    msgInfo.parent = this;
                    msgInfo.titleText = tr("Unable to write file");
                    msgInfo.descriptionText = file.errorString();
                    MessageDialogOpener::warning(msgInfo);
                }
                else
                {
                    QTextStream out(&file);
                    out << mMegaApi->exportMasterKey();

                    file.close();

                    mMegaApi->masterKeyExported();

                    MessageDialogInfo msgInfo;
                    msgInfo.parent = this;
                    msgInfo.descriptionText =
                        tr("Exporting the master key and keeping it in a secure location"
                           " enables you to set a new password without data loss.") +
                        QString::fromUtf8("\n") +
                        tr("Always keep physical control of your master key (e.g. on a"
                           " client device, external storage, or print).");
                    MessageDialogOpener::information(msgInfo);
                }
            }
        });
}

void SettingsDialog::on_bChangePassword_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_CHANGE_PASSWORD_CLICKED);

    QPointer<ChangePassword> cPassword = new ChangePassword(this);
    DialogOpener::showDialog<ChangePassword>(cPassword);
}

void SettingsDialog::on_bSessionHistory_clicked()
{
    Utilities::openUrl(QUrl(QString::fromUtf8("mega://#fm/account/history")));
}

// Folders -----------------------------------------------------------------------------------------
void SettingsDialog::updateUploadFolder()
{
    const QString defaultFolderName =
        QLatin1Char('/') + CommonMessages::getDefaultUploadFolderName();
    std::unique_ptr<MegaNode> node(
        mMegaApi->getNodeByHandle(static_cast<uint64_t>(mPreferences->uploadFolder())));
    if (!node)
    {
        mHasDefaultUploadOption = false;
        mUi->eUploadFolder->setText(defaultFolderName);
    }
    else
    {
        std::unique_ptr<const char[]> nPath(mMegaApi->getNodePath(node.get()));
        if (!nPath)
        {
            mHasDefaultUploadOption = false;
            mUi->eUploadFolder->setText(defaultFolderName);
        }
        else
        {
            mHasDefaultUploadOption = mPreferences->hasDefaultUploadFolder();
            mUi->eUploadFolder->setText(QString::fromUtf8(nPath.get()));
        }
    }
}

void SettingsDialog::updateDownloadFolder()
{
    QString downloadPath = mPreferences->downloadFolder();
    if (!downloadPath.size())
    {
        downloadPath = Utilities::getDefaultBasePath() + QLatin1Char('/') +
                       CommonMessages::getDefaultDownloadFolderName();
    }
    downloadPath = QDir::toNativeSeparators(downloadPath);
    mUi->eDownloadFolder->setText(downloadPath);
    mHasDefaultDownloadOption = mPreferences->hasDefaultDownloadFolder();
}

void SettingsDialog::on_bFolders_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_FOLDERS_TAB_CLICKED);

    emit userActivity();

    if (mUi->wStack->currentWidget() == mUi->pFolders)
    {
        return;
    }
    mUi->wStack->setCurrentWidget(mUi->pFolders);
}

void SettingsDialog::on_bUploadFolder_clicked()
{
    UploadNodeSelector* nodeSelector = new UploadNodeSelector(this);
    std::shared_ptr<mega::MegaNode> defaultNode(
        mMegaApi->getNodeByPath(mUi->eUploadFolder->text().toStdString().c_str()));
    nodeSelector->setSelectedNodeHandle(defaultNode);
    nodeSelector->setDefaultUploadOption(mHasDefaultUploadOption);
    nodeSelector->showDefaultUploadOption();

    DialogOpener::showDialog<NodeSelector>(
        nodeSelector,
        [nodeSelector, this]()
        {
            if (nodeSelector->result() == QDialog::Accepted)
            {
                MegaHandle selectedMegaFolderHandle = nodeSelector->getSelectedNodeHandle();
                std::shared_ptr<MegaNode> node(mMegaApi->getNodeByHandle(selectedMegaFolderHandle));
                if (node)
                {
                    std::unique_ptr<const char[]> nPath(mMegaApi->getNodePath(node.get()));
                    if (nPath && strlen(nPath.get()))
                    {
                        mHasDefaultUploadOption = nodeSelector->getDefaultUploadOption();
                        mUi->eUploadFolder->setText(QString::fromUtf8(nPath.get()));
                        mPreferences->setHasDefaultUploadFolder(mHasDefaultUploadOption);
                        mPreferences->setUploadFolder(node->getHandle());
                    }
                }
            }
        });
}

void SettingsDialog::on_bDownloadFolder_clicked()
{
    QPointer<DownloadFromMegaDialog> dialog =
        new DownloadFromMegaDialog(mPreferences->downloadFolder(), this);
    dialog->setDefaultDownloadOption(mHasDefaultDownloadOption);
    DialogOpener::showDialog<DownloadFromMegaDialog>(
        dialog,
        [dialog, this]()
        {
            if (dialog->result() == QDialog::Accepted)
            {
                QString fPath = dialog->getPath();
                if (!fPath.isEmpty())
                {
                    QTemporaryFile test(fPath + QDir::separator());
                    if (test.open())
                    {
                        mHasDefaultDownloadOption = dialog->isDefaultDownloadOption();
                        mUi->eDownloadFolder->setText(fPath);
                        mPreferences->setDownloadFolder(fPath);
                        mPreferences->setHasDefaultDownloadFolder(mHasDefaultDownloadOption);
                    }
                    else
                    {
                        MessageDialogInfo msgInfo;
                        msgInfo.parent = this;
                        msgInfo.descriptionText = tr("You don't have write permissions"
                                                     " in this local folder.");
                        MessageDialogOpener::critical(msgInfo);
                    }
                }
            }
        });
}

void SettingsDialog::onShellNotificationsProcessed()
{
    setOverlayCheckboxEnabled(true, mUi->cOverlayIcons->isChecked());
}

void SettingsDialog::onUserEmailChanged(mega::MegaHandle userHandle, const QString& newEmail)
{
    if (!mPreferences->logged())
    {
        return;
    }

    MegaHandle myHandle = mMegaApi->getMyUserHandleBinary();
    if (userHandle == myHandle)
    {
        mPreferences->setEmail(newEmail);
        mUi->lEmail->setText(newEmail);
    }
}

// Network -----------------------------------------------------------------------------------------
void SettingsDialog::on_bNetwork_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_NETWORK_TAB_CLICKED);

    emit userActivity();

    if (mUi->wStack->currentWidget() == mUi->pNetwork)
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pNetwork);
}

void SettingsDialog::on_bOpenProxySettings_clicked()
{
    QPointer<ProxySettings> proxySettingsDialog(new ProxySettings(mApp, this));
    DialogOpener::showDialog<ProxySettings>(proxySettingsDialog,
                                            [proxySettingsDialog, this]()
                                            {
                                                if (proxySettingsDialog->result() ==
                                                    QDialog::Accepted)
                                                {
                                                    mApp->applyProxySettings();
                                                    updateNetworkTab();
                                                }
                                            });
}

void SettingsDialog::on_bOpenBandwidthSettings_clicked()
{
    QPointer<BandwidthSettings> bandwidthSettings(new BandwidthSettings(mApp, this));
    DialogOpener::showDialog<BandwidthSettings>(
        bandwidthSettings,
        [bandwidthSettings, this]()
        {
            if (bandwidthSettings->result() == QDialog::Accepted)
            {
                mApp->setMaxUploadSpeed(mPreferences->uploadLimitKB());
                mApp->setMaxDownloadSpeed(mPreferences->downloadLimitKB());

                mApp->setMaxConnections(MegaTransfer::TYPE_UPLOAD,
                                        mPreferences->parallelUploadConnections());
                mApp->setMaxConnections(MegaTransfer::TYPE_DOWNLOAD,
                                        mPreferences->parallelDownloadConnections());

                mApp->setUseHttpsOnly(mPreferences->usingHttpsOnly());

                updateNetworkTab();
            }
        });
}

void SettingsDialog::on_bNotifications_clicked()
{
    MegaSyncApp->getStatsEventHandler()->sendTrackedEvent(
        AppStatsEvents::EventType::SETTINGS_NOTIFICATIONS_TAB_CLICKED);

    emit userActivity();

    if (mUi->wStack->currentWidget() == mUi->pNotifications)
    {
        return;
    }

    mUi->wStack->setCurrentWidget(mUi->pNotifications);
}

void SettingsDialog::updateNetworkTab()
{
    int uploadLimitKB = mPreferences->uploadLimitKB();
    if (uploadLimitKB < 0)
    {
        mUi->lUploadRateLimit->setText(
            QCoreApplication::translate("SettingsDialog_Bandwith", "Auto"));
    }
    else if (uploadLimitKB > 0)
    {
        mUi->lUploadRateLimit->setText(QStringLiteral("%1 KB/s").arg(uploadLimitKB));
    }
    else
    {
        mUi->lUploadRateLimit->setText(tr("No limit"));
    }

    int downloadLimitKB = mPreferences->downloadLimitKB();
    if (downloadLimitKB > 0)
    {
        mUi->lDownloadRateLimit->setText(QStringLiteral("%1 KB/s").arg(downloadLimitKB));
    }
    else
    {
        mUi->lDownloadRateLimit->setText(tr("No limit"));
    }

    switch (mPreferences->proxyType())
    {
        case Preferences::PROXY_TYPE_NONE:
            mUi->lProxySettings->setText(tr("No Proxy"));
            break;
        case Preferences::PROXY_TYPE_AUTO:
            mUi->lProxySettings->setText(
                QCoreApplication::translate("SettingsDialog_Proxies", "Auto"));
            break;
        case Preferences::PROXY_TYPE_CUSTOM:
            mUi->lProxySettings->setText(tr("Manual"));
            break;
    }
}

void SettingsDialog::setShortCutsForToolBarItems()
{
    // Provide quick access shortcuts for Settings panes via Ctrl+1,2,3..
    // Ctrl is auto-magically translated to CMD key by Qt on macOS
    for (int i = 0; i < mUi->wStack->count(); ++i)
    {
        QShortcut* scGeneral =
            new QShortcut(QKeySequence(QString::fromLatin1("Ctrl+%1").arg(i + 1)), this);
        QObject::connect(scGeneral,
                         &QShortcut::activated,
                         this,
                         [=]()
                         {
                             openSettingsTab(i);
                         });
    }
}

void SettingsDialog::updateCacheSchedulerDaysLabel()
{
    mUi->lCacheSchedulerSuffix->setText(tr("day", "", mPreferences->cleanerDaysLimitValue()));
}

void SettingsDialog::startRequestTaskbarPinningTimer()
{
    auto preferences = Preferences::instance();
    if (preferences->logged() &&
        !preferences->isOneTimeActionUserDone(Preferences::ONE_TIME_ACTION_REQUEST_PIN_TASKBAR))
    {
        mTaskbarPinningRequestTimer = new QTimer(this);
        connect(mTaskbarPinningRequestTimer,
                &QTimer::timeout,
                this,
                &SettingsDialog::onRequestTaskbarPinningTimeout);

        mTaskbarPinningRequestTimer->start(500ms);
    }
}

void SettingsDialog::onRequestTaskbarPinningTimeout()
{
    mTaskbarPinningRequestTimer->stop();
    Platform::getInstance()->pinOnTaskbar();
}
