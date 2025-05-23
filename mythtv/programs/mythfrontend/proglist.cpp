// C/C++
#include <algorithm>
#include <deque>                        // for _Deque_iterator, operator-, etc
#include <functional>
#include <iterator>                     // for reverse_iterator
#include <utility>

// Qt
#include <QCoreApplication>
#include <QLocale>

// MythTV
#include "libmythbase/mythcorecontext.h"
#include "libmythbase/mythdate.h"
#include "libmythbase/mythdb.h"
#include "libmythbase/mythlogging.h"
#include "libmythbase/stringutil.h"
#include "libmythtv/channelinfo.h"
#include "libmythtv/channelutil.h"
#include "libmythtv/recordinginfo.h"
#include "libmythtv/recordingrule.h"
#include "libmythtv/scheduledrecording.h"
#include "libmythtv/tv_actions.h"       // for ACTION_CHANNELSEARCH
#include "libmythtv/tv_play.h"
#include "libmythui/mythdialogbox.h"
#include "libmythui/mythuibuttonlist.h"
#include "libmythui/mythuistatetype.h"
#include "libmythui/mythuitext.h"

// MythFrontend
#include "proglist.h"

#define LOC      QString("ProgLister: ")
#define LOC_WARN QString("ProgLister, Warning: ")
#define LOC_ERR  QString("ProgLister, Error: ")

void *ProgLister::RunProgramList(void *player, ProgListType pltype,
                                 const QString & extraArg)
{
    MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();
    auto *vsb = new ProgLister(mainStack, static_cast<TV*>(player),
                               pltype, extraArg);

    if (vsb->Create())
        mainStack->AddScreen(vsb, (player == nullptr));
    else
        delete vsb;

    return nullptr;
}

ProgLister::ProgLister(MythScreenStack *parent, ProgListType pltype,
                       QString view, QString extraArg,
                       QDateTime selectedTime) :
    ScheduleCommon(parent, "ProgLister"),
    m_type(pltype),
    m_extraArg(std::move(extraArg)),
    m_startTime(MythDate::current()),
    m_searchTime(m_startTime),
    m_selectedTime(std::move(selectedTime)),
    m_channelOrdering(gCoreContext->GetSetting("ChannelOrdering", "channum")),
    m_view(std::move(view))
{
    if (pltype == plMovies)
    {
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT COUNT(*) FROM program WHERE stars > 0");

        if (query.exec() && query.next())
        {
            if (query.value(0).toInt() == 0) // No ratings in database
            {
                m_curView = 0; // Show All
                m_allowViewDialog = false;
            }
        }
    }

    switch (pltype)
    {
        case plTitleSearch:   m_searchType = kTitleSearch;   break;
        case plKeywordSearch: m_searchType = kKeywordSearch; break;
        case plPeopleSearch:  m_searchType = kPeopleSearch;  break;
        case plPowerSearch:
        case plSQLSearch:
        case plStoredSearch:  m_searchType = kPowerSearch;   break;
        default:              m_searchType = kNoSearch;      break;
    }
}

// From tv_play
ProgLister::ProgLister(MythScreenStack *parent, TV* player,
                       ProgListType pltype, const QString & extraArg) :
    ScheduleCommon(parent, "ProgLister"),
    m_type(pltype),
    m_extraArg(extraArg),
    m_startTime(MythDate::current()),
    m_searchTime(m_startTime),
    m_channelOrdering(gCoreContext->GetSetting("ChannelOrdering", "channum")),
    m_player(player)
{
    if (m_player)
        m_player->IncrRef();

    switch (pltype)
    {
        case plTitleSearch:   m_searchType = kTitleSearch;   break;
        case plKeywordSearch: m_searchType = kKeywordSearch; break;
        case plPeopleSearch:  m_searchType = kPeopleSearch;  break;
        case plPowerSearch:
        case plSQLSearch:
        case plStoredSearch:  m_searchType = kPowerSearch;   break;
        default:              m_searchType = kNoSearch;      break;
    }

    m_view = extraArg;
    m_viewList.push_back(extraArg);
    m_viewTextList.push_back(extraArg);
    m_curView = m_viewList.size() - 1;
}

// previously recorded ctor
ProgLister::ProgLister(
    MythScreenStack *parent, uint recid, QString title) :
    ScheduleCommon(parent, "PreviousList"),
    m_type(plPreviouslyRecorded),
    m_recid(recid),
    m_title(std::move(title)),
    m_startTime(MythDate::current()),
    m_searchTime(m_startTime),
    m_channelOrdering(gCoreContext->GetSetting("ChannelOrdering", "channum")),
    m_view("reverse time"),
    m_reverseSort(true)
{
}

ProgLister::~ProgLister(void)
{
    m_itemList.clear();
    m_itemListSave.clear();
    gCoreContext->removeListener(this);

    // if we have a player, we need to tell we are done
    if (m_player)
    {
        emit m_player->RequestEmbedding(false);
        m_player->DecrRef();
    }
}

void ProgLister::Close(void)
{
    // don't fade the screen if we are returning to the player
    if (m_player)
        GetScreenStack()->PopScreen(this, false);
    else
        GetScreenStack()->PopScreen(this, true);
}

bool ProgLister::Create(void)
{
    if (!LoadWindowFromXML("schedule-ui.xml", "programlist", this))
        return false;

    bool err = false;
    UIUtilW::Assign(this, m_curviewText, "curview", &err);
    UIUtilE::Assign(this, m_progList, "proglist", &err);
    UIUtilW::Assign(this, m_schedText, "sched", &err);
    UIUtilW::Assign(this, m_messageText, "msg", &err);
    UIUtilW::Assign(this, m_positionText, "position", &err);
    UIUtilW::Assign(this, m_groupByText, "groupby");

    if (err)
    {
        LOG(VB_GENERAL, LOG_ERR, "Cannot load screen 'programlist'");
        return false;
    }

    connect(m_progList, &MythUIButtonList::itemSelected,
            this,       &ProgLister::HandleSelected);

    connect(m_progList, &MythUIButtonList::itemVisible,
            this,       &ProgLister::HandleVisible);

    connect(m_progList, &MythUIButtonList::itemLoaded,
            this,       &ProgLister::HandleVisible);

    if (m_type == plPreviouslyRecorded)
    {
        connect(m_progList, &MythUIButtonList::itemClicked,
                this,       &ProgLister::ShowOldRecordedMenu);
    }
    else
    {
        connect(m_progList, &MythUIButtonList::itemClicked,
                this,       qOverload<MythUIButtonListItem*>(&ProgLister::EditRecording));
    }

    m_progList->SetLCDTitles(tr("Program List"), "title|channel|shortstarttimedate");
    m_progList->SetSearchFields("titlesubtitle");

    BuildFocusList();

    QString value;
    switch (m_type)
    {
        case plTitle:              value = tr("Program Listings"); break;
        case plNewListings:        value = tr("New Title Search"); break;
        case plTitleSearch:        value = tr("Title Search");     break;
        case plKeywordSearch:      value = tr("Keyword Search");   break;
        case plPeopleSearch:       value = tr("People Search");    break;
        case plStoredSearch:       value = tr("Stored Search");    break;
        case plPowerSearch:
        case plSQLSearch:          value = tr("Power Search");     break;
        case plRecordid:           value = tr("Rule Search");      break;
        case plCategory:           value = tr("Category Search");  break;
        case plChannel:            value = tr("Channel Search");   break;
        case plMovies:             value = tr("Movie Search");     break;
        case plTime:               value = tr("Time Search");      break;
        case plPreviouslyRecorded: value = tr("Previously Recorded"); break;
        default:                   value = tr("Unknown Search");   break;
    }

    if (m_schedText)
        m_schedText->SetText(value);

    gCoreContext->addListener(this);

    LoadInBackground();

    if (m_player)
        emit m_player->RequestEmbedding(true);

    return true;
}

void ProgLister::Load(void)
{
    if (m_viewList.isEmpty() || m_curView < 0)
        FillViewList(m_view);

    FillItemList(false, false);

    auto *slce = new ScreenLoadCompletionEvent(objectName());
    QCoreApplication::postEvent(this, slce);
}

bool ProgLister::keyPressEvent(QKeyEvent *e)
{
    if (!m_allowEvents)
        return true;

    if (GetFocusWidget() && GetFocusWidget()->keyPressEvent(e))
    {
        m_allowEvents = true;
        return true;
    }

    m_allowEvents = false;

    QStringList actions;
    bool handled = GetMythMainWindow()->TranslateKeyPress(
        "TV Frontend", e, actions);

    bool needUpdate = false;
    for (uint i = 0; i < uint(actions.size()) && !handled; ++i)
    {
        const QString& action = actions[i];
        handled = true;

        if (action == "PREVVIEW")
            SwitchToPreviousView();
        else if (action == "NEXTVIEW")
            SwitchToNextView();
        else if (action == "CUSTOMEDIT")
            EditCustom();
        else if (action == "EDIT")
            EditScheduled();
        else if (action == "DELETE")
            ShowDeleteItemMenu();
        else if (action == "UPCOMING" && m_type != plTitle)
            ShowUpcoming();
        else if (action == "PREVRECORDED" && m_type != plPreviouslyRecorded)
            ShowPrevious();
        else if (action == "DETAILS" || action == "INFO")
            ShowDetails();
        else if (action == "GUIDE")
            ShowGuide();
        else if (action == ACTION_CHANNELSEARCH && m_type != plChannel)
            ShowChannelSearch();
        else if (action == "TOGGLERECORD")
            QuickRecord();
        else if (action == "1")
        {
            if (m_titleSort)
            {
                m_titleSort = false;
                m_reverseSort = (m_type == plPreviouslyRecorded);
            }
            else
            {
                m_reverseSort = !m_reverseSort;
            }
            needUpdate = true;
        }
        else if (action == "2")
        {
            if (!m_titleSort)
            {
                m_titleSort = true;
                m_reverseSort = false;
            }
            else
            {
                m_reverseSort = !m_reverseSort;
            }
            needUpdate = true;
        }
        else
        {
            handled = false;
        }
    }

    if (!handled && MythScreenType::keyPressEvent(e))
        handled = true;

    if (needUpdate)
        LoadInBackground();

    m_allowEvents = true;

    return handled;
}

void ProgLister::ShowMenu(void)
{
    auto *sortGroupMenu = new MythMenu(tr("Sort/Group Options"), this,
                                       "sortgroupmenu");
    sortGroupMenu->AddItem(tr("Reverse Sort Order"));
    sortGroupMenu->AddItem(tr("Sort By Title"));
    sortGroupMenu->AddItem(tr("Sort By Time"));

    auto *menu = new MythMenu(tr("Options"), this, "menu");

    if (m_allowViewDialog && m_type != plPreviouslyRecorded)
    {
        menu->AddItem(tr("Choose Search Phrase..."), &ProgLister::ShowChooseViewMenu);
    }

    if (m_type != plChannel
        && m_type != plTime
        && m_type != plPreviouslyRecorded)
    {
        AddGroupMenuItems(sortGroupMenu);
    }

    menu->AddItem(tr("Sort/Group"), nullptr, sortGroupMenu);

    if (m_type != plPreviouslyRecorded)
        menu->AddItem(tr("Record"), &ProgLister::QuickRecord);

    menu->AddItem(tr("Edit Schedule"),   qOverload<>(&ProgLister::EditScheduled));
    menu->AddItem(tr("Program Details"), &ProgLister::ShowDetails);
    menu->AddItem(tr("Program Guide"),   &ProgLister::ShowGuide);
    if (m_type != plChannel)
        menu->AddItem(tr("Channel Search"), &ProgLister::ShowChannelSearch);
    if (m_type != plTitle)
        menu->AddItem(tr("Upcoming"),    qOverload<>(&ProgLister::ShowUpcoming));
    if (m_type != plPreviouslyRecorded)
        menu->AddItem(tr("Previously Recorded"), qOverload<>(&ProgLister::ShowPrevious));
    menu->AddItem(tr("Custom Edit"),     &ProgLister::EditCustom);

    ProgramInfo *pi = m_itemList[m_progList->GetCurrentPos()];
    if (m_type != plPreviouslyRecorded)
    {
        if (pi && pi->GetRecordingRuleID())
            menu->AddItem(tr("Delete Rule"), &ProgLister::ShowDeleteRuleMenu);
    }
    else
    {
        menu->AddItem(
            tr("Delete Episode"), &ProgLister::ShowDeleteOldEpisodeMenu);
    }

    MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");
    auto *menuPopup = new MythDialogBox(menu, popupStack, "menuPopup");

    if (!menuPopup->Create())
    {
        delete menuPopup;
        return;
    }

    popupStack->AddScreen(menuPopup);
}

void ProgLister::SwitchToPreviousView(void)
{
    if (m_type == plTime && !m_viewList.empty() && !m_viewTextList.empty())
    {
        m_searchTime = m_searchTime.addSecs(-3600);
        m_curView = 0;
        m_viewList[m_curView]     = MythDate::toString(m_searchTime,
                                                         MythDate::kDateTimeFull | MythDate::kSimplify);
        m_viewTextList[m_curView] = m_viewList[m_curView];
        LoadInBackground();
        return;
    }

    if (m_viewList.size() <= 1)
        return;

    m_curView--;
    if (m_curView < 0)
        m_curView = m_viewList.size() - 1;

    LoadInBackground();
}

void ProgLister:: SwitchToNextView(void)
{
    if (m_type == plTime && !m_viewList.empty() && !m_viewTextList.empty())
    {
        m_searchTime = m_searchTime.addSecs(3600);
        m_curView = 0;
        m_viewList[m_curView] = MythDate::toString(m_searchTime,
                                                     MythDate::kDateTimeFull | MythDate::kSimplify);
        m_viewTextList[m_curView] = m_viewList[m_curView];
        LoadInBackground();

        return;
    }

    if (m_viewList.size() <= 1)
        return;

    m_curView++;
    if (m_curView >= m_viewList.size())
        m_curView = 0;

    LoadInBackground();
}

void ProgLister::UpdateKeywordInDB(const QString &text, const QString &oldValue)
{
    int oldview = m_viewList.indexOf(oldValue);
    int newview = m_viewList.indexOf(text);

    if (newview >= 0 && newview == oldview)
        return;

    if (oldview >= 0)
    {
        QString qphrase = m_viewList[oldview];

        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("DELETE FROM keyword "
                      "WHERE phrase = :PHRASE AND searchtype = :TYPE;");
        query.bindValue(":PHRASE", qphrase);
        query.bindValue(":TYPE", m_searchType);
        if (!query.exec())
        {
            MythDB::DBError(
                "ProgLister::updateKeywordInDB -- delete", query);
        }
        m_viewList.removeAll(qphrase);
        m_viewTextList.removeAll(qphrase);
    }

    if (newview < 0)
    {
        const QString& qphrase = text;

        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("REPLACE INTO keyword (phrase, searchtype)"
                      "VALUES(:PHRASE, :TYPE );");
        query.bindValue(":PHRASE", qphrase);
        query.bindValue(":TYPE", m_searchType);
        if (!query.exec())
        {
            MythDB::DBError(
                "ProgLister::updateKeywordInDB -- replace", query);
        }
        m_viewList.push_back(qphrase);
        m_viewTextList.push_back(qphrase);
    }
}

void ProgLister::ShowChooseViewMenu(void)
{
    MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");
    MythScreenType *screen = nullptr;

    switch (m_type)
    {
        case plChannel:
        case plCategory:
        case plMovies:
        case plNewListings:
        case plStoredSearch:
        {
            if (m_viewList.empty())
                return;

            QString msg;
            switch (m_type)
            {
                case plMovies: msg = tr("Select Rating"); break;
                case plChannel: msg = tr("Select Channel"); break;
                case plCategory: msg = tr("Select Category"); break;
                case plNewListings: msg = tr("Select List"); break;
                case plStoredSearch: msg = QString("%1\n%2")
                    .arg(tr("Select a search stored from"),
                         tr("Custom Record")); break;
                default: // silence warning
                    break;
            }

            auto *dialog = new MythUISearchDialog(
                popupStack, msg, m_viewTextList, true, "");
            if (!dialog)
                return;
            connect(dialog, &MythUISearchDialog::haveResult,
                    this, &ProgLister::SetViewFromList);
            screen = dialog;
            break;
        }
        case plTitleSearch:
        case plKeywordSearch:
        case plPeopleSearch:
        {
            auto *dialog = new PhrasePopup(
                popupStack, this, m_searchType, m_viewTextList,
                (m_curView >= 0) ? m_viewList[m_curView] : QString());
            if (!dialog)
                return;
            connect(dialog, &PhrasePopup::haveResult,
                    this, &ProgLister::SetViewFromList);
            screen = dialog;
            break;
        }
        case plPowerSearch:
        {
            auto *dialog = new PowerSearchPopup(
                popupStack, this, m_searchType, m_viewTextList,
                (m_curView >= 0) ? m_viewList[m_curView] : QString());
            if (!dialog)
                return;
            connect(dialog, &PowerSearchPopup::haveResult,
                    this, &ProgLister::SetViewFromList);
            screen = dialog;
            break;
        }
        case plTime:
        {
            QString message =  tr("Start search from date and time");
            int flags = (MythTimeInputDialog::kDay |
                         MythTimeInputDialog::kHours |
                         MythTimeInputDialog::kFutureDates);
            auto *dialog = new MythTimeInputDialog(popupStack, message, flags);
            if (!dialog)
                return;
            connect(dialog, &MythTimeInputDialog::haveResult,
                    this, &ProgLister::SetViewFromTime);
            screen = dialog;
            break;
        }
        case plRecordid:
        case plPreviouslyRecorded:
        case plUnknown:
        case plTitle:
        case plSQLSearch:
            break;
    }

    if (!screen)
    {
        LOG(VB_GENERAL, LOG_WARNING, LOC + "No menu");
        return;
    }

    if (!screen->Create())
    {
        delete screen;
        return;
    }

    popupStack->AddScreen(screen);
}

void ProgLister::SetViewFromTime(QDateTime searchTime)
{
    if (m_viewList.empty() || m_viewTextList.empty())
        return;

    m_searchTime = std::move(searchTime);
    m_curView = 0;
    m_viewList[m_curView] = MythDate::toString(m_searchTime,
                                                 MythDate::kDateTimeFull | MythDate::kSimplify);
    m_viewTextList[m_curView] = m_viewList[m_curView];

    LoadInBackground();
}

void ProgLister::SetViewFromList(const QString& item)
{
    m_curView = m_viewTextList.indexOf(item);
    if (m_curView >= 0)
        LoadInBackground();
}

bool ProgLister::PowerStringToSQL(
    const QString &qphrase, QString &output, MSqlBindings &bindings)
{
    output.clear();

    QStringList field = qphrase.split(':');
    if (field.size() != 6)
    {
        LOG(VB_GENERAL, LOG_ERR, LOC + "Power search should have 6 fields," +
            QString("\n\t\t\tnot %1 (%2)") .arg(field.size()).arg(qphrase));
        return false;
    };

    static const std::array<QString,6> kBindingList
    {
        ":POWERTITLE",
        ":POWERSUB",
        ":POWERDESC",
        ":POWERCATTYPE",
        ":POWERGENRE",
        ":POWERCALLSIGN",
    };

    static const std::array<QString,6> kOutputList
    {
        "program.title LIKE :POWERTITLE ",
        "program.subtitle LIKE :POWERSUB ",
        "program.description LIKE :POWERDESC ",
        "program.category_type = :POWERCATTYPE ",
        "programgenres.genre = :POWERGENRE ",
        "channel.callsign = :POWERCALLSIGN ",
    };

    for (uint i = 0; i < (uint) field.size(); i++)
    {
        if (field[i].isEmpty())
            continue;

        if (!output.isEmpty())
            output += "\nAND ";

        output += kOutputList[i];
        bindings[kBindingList[i]] =
            (!kOutputList[i].contains("=")) ?
            QString('%') + field[i] + QString('%') : field[i];
    }

    return output.contains("programgenres");
}

ProgramInfo *ProgLister::GetCurrentProgram(void) const
{
    int pos = m_progList->GetCurrentPos();
    if (pos >= 0 && pos < (int) m_itemList.size())
        return m_itemList[pos];
    return nullptr;
}

void ProgLister::ShowDeleteItemMenu(void)
{
    if (m_type == plPreviouslyRecorded)
        ShowDeleteOldEpisodeMenu();
    else
        ShowDeleteRuleMenu();
}

void ProgLister::ShowDeleteRuleMenu(void)
{
    ProgramInfo *pi = GetCurrentProgram();

    if (!pi || !pi->GetRecordingRuleID())
        return;

    auto *record = new RecordingRule();
    if (!record->LoadByProgram(pi))
    {
        delete record;
        return;
    }

    QString message = tr("Delete '%1' %2 rule?")
        .arg(record->m_title, toString(pi->GetRecordingRuleType()));

    MythScreenStack *popupStack = GetMythMainWindow()->GetStack("popup stack");

    auto *okPopup = new MythConfirmationDialog(
        popupStack, message, true);

    okPopup->SetReturnEvent(this, "deleterule");
    okPopup->SetData(QVariant::fromValue(record));

    if (okPopup->Create())
        popupStack->AddScreen(okPopup);
    else
        delete okPopup;
}

void ProgLister::ShowDeleteOldEpisodeMenu(void)
{
    ProgramInfo *pi = GetCurrentProgram();

    if (!pi)
        return;

    QString message = tr("Delete this episode of '%1'?").arg(pi->GetTitle());

    ShowOkPopup(message, this, &ProgLister::DeleteOldEpisode, true);
}

void ProgLister::DeleteOldEpisode(bool ok)
{
    ProgramInfo *pi = GetCurrentProgram();
    if (!ok || !pi)
        return;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare(
        "DELETE FROM oldrecorded "
        "WHERE chanid    = :CHANID AND "
        "      starttime = :STARTTIME");
    query.bindValue(":CHANID",    pi->GetChanID());
    query.bindValue(":STARTTIME", pi->GetScheduledStartTime());

    if (!query.exec())
        MythDB::DBError("ProgLister::DeleteOldEpisode", query);

    ScheduledRecording::RescheduleCheck(*pi, "DeleteOldEpisode");
    FillItemList(true);
}

void ProgLister::ShowDeleteOldSeriesMenu(void)
{
    ProgramInfo *pi = GetCurrentProgram();

    if (!pi)
        return;

    QString message = tr("Delete all episodes of '%1'?").arg(pi->GetTitle());

    ShowOkPopup(message, this, &ProgLister::DeleteOldSeries, true);
}

void ProgLister::DeleteOldSeries(bool ok)
{
    ProgramInfo *pi = GetCurrentProgram();
    if (!ok || !pi)
        return;

    MSqlQuery query(MSqlQuery::InitCon());
    query.prepare("DELETE FROM oldrecorded "
                  "WHERE title = :TITLE AND future = 0");
    query.bindValue(":TITLE", pi->GetTitle());
    if (!query.exec())
        MythDB::DBError("ProgLister::DeleteOldSeries -- delete", query);

    // Set the programid to the special value of "**any**" which the
    // scheduler recognizes to mean the entire series was deleted.
    RecordingInfo tempri(*pi);
    tempri.SetProgramID("**any**");
    ScheduledRecording::RescheduleCheck(tempri, "DeleteOldSeries");
    FillItemList(true);
}

void ProgLister::ShowOldRecordedMenu(void)
{
    ProgramInfo *pi = GetCurrentProgram();

    if (!pi)
        return;

    QString message = pi->toString(ProgramInfo::kTitleSubtitle, " - ");

    if (!pi->GetDescription().isEmpty())
        message += "\n\n" + pi->GetDescription();

    message += "\n\n\n" + tr("NOTE: removing items from this list will not "
                             "delete any recordings.");

    QString title = tr("Previously Recorded");

    auto *menu = new MythMenu(title, message, this, "deletemenu");
    if (pi->IsDuplicate())
        menu->AddItem(tr("Allow this episode to re-record"));
    else
        menu->AddItem(tr("Never record this episode"));
    menu->AddItem(tr("Remove this episode from the list"));
    menu->AddItem(tr("Remove all episodes for this title"));
    menu->AddItem(tr("Cancel"));

    MythScreenStack *mainStack = GetMythMainWindow()->GetMainStack();
    auto *menuPopup = new MythDialogBox(menu, mainStack, "deletepopup", true);

    if (menuPopup->Create())
        mainStack->AddScreen(menuPopup);
    else
        delete menuPopup;
}

void ProgLister::FillViewList(const QString &view)
{
    m_viewList.clear();
    m_viewTextList.clear();

    if (m_type == plChannel) // list by channel
    {
        ChannelInfoList channels = ChannelUtil::GetChannels(
            0, true, "channum, chanid");
        ChannelUtil::SortChannels(channels, m_channelOrdering, true);

        for (auto & channel : channels)
        {
            QString chantext = channel.GetFormatted(ChannelInfo::kChannelShort);

            m_viewList.push_back(QString::number(channel.m_chanId));
            m_viewTextList.push_back(chantext);
        }

        if (!view.isEmpty())
            m_curView = m_viewList.indexOf(view);
    }
    else if (m_type == plCategory) // list by category
    {
        QDateTime query_starttime = m_startTime.addSecs(50 -
                m_startTime.time().second());
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT g1.genre, g2.genre "
                      "FROM program "
                      "JOIN programgenres g1 ON "
                      "  program.chanid = g1.chanid AND "
                      "  program.starttime = g1.starttime "
                      "LEFT JOIN programgenres g2 ON "
                      "  g1.chanid = g2.chanid AND "
                      "  g1.starttime = g2.starttime "
                      "WHERE program.endtime > :PGILSTART "
                      "GROUP BY g1.genre, g2.genre;");
        query.bindValue(":PGILSTART", query_starttime);

        m_useGenres = false;

        if (query.exec())
        {
            QString lastGenre1;

            while (query.next())
            {
                m_useGenres = true;

                QString genre1 = query.value(0).toString();
                if (genre1.isEmpty())
                    continue;

                if (genre1 != lastGenre1)
                {
                    m_viewList.push_back(genre1);
                    m_viewTextList.push_back(genre1);
                    lastGenre1 = genre1;
                }

                QString genre2 = query.value(1).toString();
                if (genre2.isEmpty() || genre2 == genre1)
                    continue;

                m_viewList.push_back(genre1 + ":/:" + genre2);
                m_viewTextList.push_back("    " + genre1 + " / " + genre2);
            }
        }

        if (!m_useGenres)
        {
            query.prepare("SELECT category "
                          "FROM program "
                          "WHERE program.endtime > :PGILSTART "
                          "GROUP BY category");
            query.bindValue(":PGILSTART", query_starttime);

            if (query.exec())
            {
                while (query.next())
                {
                    QString category = query.value(0).toString();
                    if (category.isEmpty())
                        continue;
                    category = query.value(0).toString();
                    m_viewList.push_back(category);
                    m_viewTextList.push_back(category);
                }
            }
        }

        if (!view.isEmpty())
            m_curView = m_viewList.indexOf(view);
    }
    else if (m_type == plTitleSearch || m_type == plKeywordSearch ||
             m_type == plPeopleSearch || m_type == plPowerSearch)
    {
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT phrase FROM keyword "
                      "WHERE searchtype = :SEARCHTYPE;");
        query.bindValue(":SEARCHTYPE", m_searchType);

        if (query.exec())
        {
            while (query.next())
            {
                /* The keyword.phrase column uses utf8_bin collation, so
                 * Qt uses QString::fromAscii() for toString(). Explicitly
                 * convert the value using QString::fromUtf8() to prevent
                 * corruption. */
                QString phrase = QString::fromUtf8(query.value(0)
                                                   .toByteArray().constData());
                if (phrase.isEmpty())
                    continue;
                m_viewList.push_back(phrase);
                m_viewTextList.push_back(phrase);
            }
        }

        if (!view.isEmpty())
        {
            m_curView = m_viewList.indexOf(view);

            if (m_curView < 0)
            {
                const QString& qphrase = view;

                MSqlQuery query2(MSqlQuery::InitCon());
                query2.prepare("REPLACE INTO keyword (phrase, searchtype)"
                              "VALUES(:VIEW, :SEARCHTYPE );");
                query2.bindValue(":VIEW", qphrase);
                query2.bindValue(":SEARCHTYPE", m_searchType);
                if (!query2.exec())
                    MythDB::DBError("ProgLister::FillViewList -- "
                                    "replace keyword", query2);

                m_viewList.push_back(qphrase);
                m_viewTextList.push_back(qphrase);

                m_curView = m_viewList.size() - 1;
            }
        }
        else
        {
            LOG(VB_GENERAL, LOG_WARNING, LOC + QString("Failed to load viewList"));
            m_curView = -1;
        }
    }
    else if (m_type == plTitle)
    {
        if (!view.isEmpty())
        {
            m_viewList.push_back(view);
            m_viewTextList.push_back(view);
            m_curView = 0;
        }
        else
        {
            m_curView = -1;
        }
    }
    else if (m_type == plNewListings)
    {
        m_viewList.push_back("all");
        m_viewTextList.push_back(tr("All"));

        m_viewList.push_back("premieres");
        m_viewTextList.push_back(tr("Premieres"));

        m_viewList.push_back("movies");
        m_viewTextList.push_back(tr("Movies"));

        m_viewList.push_back("series");
        m_viewTextList.push_back(tr("Series"));

        m_viewList.push_back("specials");
        m_viewTextList.push_back(tr("Specials"));

        if (!view.isEmpty())
            m_curView = m_viewList.indexOf(view);
    }
    else if (m_type == plMovies)
    {
        m_viewList.push_back(">= 0.0");
        m_viewTextList.push_back(tr("All"));
        m_viewList.push_back("= 0.0");
        m_viewTextList.push_back(tr("Unrated"));
        m_viewList.push_back(QString(">= %1").arg(((10 - 0.5) / 10.0) - 0.001));
        m_viewTextList.push_back(tr("%n star(s)", "", 10));
        for (int i = 9; i > 0; i--)
        {
            float stars = ((i - 0.5F ) / 10.0F) - 0.001F;
            m_viewList.push_back(QString(">= %1").arg(stars));
            m_viewTextList.push_back(tr("%n star(s) and above", "", i));
        }

        if (!view.isEmpty())
            m_curView = m_viewList.indexOf(view);
    }
    else if (m_type == plTime)
    {
        m_curView = 0;
        m_viewList.push_back(MythDate::toString(m_searchTime,
                                                  MythDate::kDateTimeFull | MythDate::kSimplify));
        m_viewTextList.push_back(m_viewList[m_curView]);
    }
    else if (m_type == plSQLSearch)
    {
        m_curView = 0;
        m_viewList.push_back(view);
        m_viewTextList.push_back(tr("Power Recording Rule"));
    }
    else if (m_type == plRecordid)
    {
        m_curView = 0;

        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT title FROM record "
                      "WHERE recordid = :RECORDID");
        query.bindValue(":RECORDID", view);

        if (query.exec() && query.next())
        {
            QString title = query.value(0).toString();
            m_viewList.push_back(view);
            m_viewTextList.push_back(title);
        }
    }
    else if (m_type == plStoredSearch) // stored searches
    {
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT rulename FROM customexample "
                      "WHERE search > 0 ORDER BY rulename;");

        if (query.exec())
        {
            while (query.next())
            {
                QString rulename = query.value(0).toString();
                if (rulename.isEmpty() || rulename.trimmed().isEmpty())
                    continue;
                rulename = query.value(0).toString();
                m_viewList.push_back(rulename);
                m_viewTextList.push_back(rulename);
            }
        }
        if (!view.isEmpty())
            m_curView = m_viewList.indexOf(view);
    }
    else if (m_type == plPreviouslyRecorded) // previously recorded
    {
        m_viewList.push_back("sort by time");
        m_viewTextList.push_back(tr("Time"));

        m_viewList.push_back("reverse time");
        m_viewTextList.push_back(tr("Reverse Time"));

        m_viewList.push_back("sort by title");
        m_viewTextList.push_back(tr("Title"));

        m_viewList.push_back("reverse title");
        m_viewTextList.push_back(tr("Reverse Title"));

        if (!view.isEmpty())
            m_curView = m_viewList.indexOf(view);
    }

    if (m_curView >= m_viewList.size())
        m_curView = m_viewList.size() - 1;
}

static bool plTitleSort(const ProgramInfo *a, const ProgramInfo *b)
{
    if (a->GetSortTitle() != b->GetSortTitle())
        return StringUtil::naturalCompare(a->GetSortTitle(), b->GetSortTitle()) < 0;
    if (a->GetSortSubtitle() != b->GetSortSubtitle())
        return StringUtil::naturalCompare(a->GetSortSubtitle(), b->GetSortSubtitle()) < 0;

    if (a->GetRecordingStatus() == b->GetRecordingStatus())
        return a->GetScheduledStartTime() < b->GetScheduledStartTime();

    if (a->GetRecordingStatus() == RecStatus::Recording ||
        a->GetRecordingStatus() == RecStatus::Tuning ||
        a->GetRecordingStatus() == RecStatus::Failing)
        return true;
    if (b->GetRecordingStatus() == RecStatus::Recording ||
        b->GetRecordingStatus() == RecStatus::Tuning ||
        b->GetRecordingStatus() == RecStatus::Failing)
        return false;

    if (a->GetRecordingStatus() == RecStatus::WillRecord ||
        a->GetRecordingStatus() == RecStatus::Pending)
        return true;
    if (b->GetRecordingStatus() == RecStatus::WillRecord ||
        b->GetRecordingStatus() == RecStatus::Pending)
        return false;

    return a->GetScheduledStartTime() < b->GetScheduledStartTime();
};

static bool plPrevTitleSort(const ProgramInfo *a, const ProgramInfo *b)
{
    if (a->GetSortTitle() != b->GetSortTitle())
        return StringUtil::naturalCompare(a->GetSortTitle(), b->GetSortTitle()) < 0;
    if (a->GetSortSubtitle() != b->GetSortSubtitle())
        return StringUtil::naturalCompare(a->GetSortSubtitle(), b->GetSortSubtitle()) < 0;

    if (a->GetProgramID() != b->GetProgramID())
        return a->GetProgramID() < b->GetProgramID();

    return a->GetScheduledStartTime() < b->GetScheduledStartTime();
};

static bool plTimeSort(const ProgramInfo *a, const ProgramInfo *b)
{
    if (a->GetScheduledStartTime() == b->GetScheduledStartTime())
        return (a->GetChanID() < b->GetChanID());

    return (a->GetScheduledStartTime() < b->GetScheduledStartTime());
};

void ProgLister::FillItemList(bool restorePosition, bool updateDisp)
{
    ProgGroupBy::Type groupBy = GetProgramListGroupBy();
    if (m_groupByText)
	m_groupByText->SetText(ProgGroupBy::toString(groupBy));

    if (m_type == plPreviouslyRecorded && m_curviewText)
    {
        if (!m_titleSort)
        {
            if (!m_reverseSort)
                m_curView = 0;
            else
                m_curView = 1;
        }
        else
        {
            if (!m_reverseSort)
                m_curView = 2;
            else
                m_curView = 3;
        }
    }

    if (m_curView < 0)
        return;

    QString where;
    QString qphrase = m_viewList[m_curView];

    MSqlBindings bindings;

    if (m_type != plPreviouslyRecorded)
        bindings[":PGILSTART"] =
            m_startTime.addSecs(50 - m_startTime.time().second());

    if (m_type == plTitle) // per title listings
    {
        where = "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND (program.title = :PGILPHRASE0 OR "
            "       (program.seriesid <> '' AND "
            "        program.seriesid = :PGILPHRASE1)) ";
        bindings[":PGILPHRASE0"] = qphrase;
        bindings[":PGILPHRASE1"] = m_extraArg;
    }
    else if (m_type == plNewListings) // what's new list
    {
        where = "LEFT JOIN oldprogram ON "
            "  oldprogram.oldtitle = program.title "
            "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND oldprogram.oldtitle IS NULL "
            "  AND program.manualid = 0 ";

        if (qphrase == "premieres")
        {
            where += "  AND ( ";
            where += "    ( program.originalairdate = DATE(";
            where += "        CONVERT_TZ(program.starttime, 'Etc/UTC', 'SYSTEM'))";
            where += "      AND (program.category = 'Special' ";
            where += "        OR program.programid LIKE 'EP%0001')) ";
            where += "    OR (program.category_type='movie' ";
            where += "      AND program.stars > 0.5 ";
            where += "      AND program.airdate >= YEAR(NOW()) - 2) ";
            where += "  ) ";
        }
        else if (qphrase == "movies")
        {
            where += "  AND program.category_type = 'movie' ";
        }
        else if (qphrase == "series")
        {
            where += "  AND program.category_type = 'series' ";
        }
        else if (qphrase == "specials")
        {
            where += "  AND program.category_type = 'tvshow' ";
        }
        else
        {
            where += "  AND (program.category_type <> 'movie' ";
            where += "  OR program.airdate >= YEAR(NOW()) - 3) ";
        }
    }
    else if (m_type == plTitleSearch) // keyword search
    {
        where = "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND program.title LIKE :PGILLIKEPHRASE0 ";
        bindings[":PGILLIKEPHRASE0"] = QString("%") + qphrase + '%';
    }
    else if (m_type == plKeywordSearch) // keyword search
    {
        where = "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND (program.title LIKE :PGILLIKEPHRASE1 "
            "    OR program.subtitle LIKE :PGILLIKEPHRASE2 "
            "    OR program.description LIKE :PGILLIKEPHRASE3 ) ";
        bindings[":PGILLIKEPHRASE1"] = QString("%") + qphrase + '%';
        bindings[":PGILLIKEPHRASE2"] = QString("%") + qphrase + '%';
        bindings[":PGILLIKEPHRASE3"] = QString("%") + qphrase + '%';
    }
    else if (m_type == plPeopleSearch) // people search
    {
        where = ", people, credits "
            "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND people.name LIKE :PGILPHRASE1 "
            "  AND credits.person = people.person "
            "  AND program.chanid = credits.chanid "
            "  AND program.starttime = credits.starttime";
        bindings[":PGILPHRASE1"] = qphrase;
    }
    else if (m_type == plPowerSearch) // complex search
    {
        QString powerWhere;
        MSqlBindings powerBindings;

        bool genreflag = PowerStringToSQL(qphrase, powerWhere, powerBindings);

        if (!powerWhere.isEmpty())
        {
            if (genreflag)
            {
                where = QString("LEFT JOIN programgenres ON "
                                "program.chanid = programgenres.chanid AND "
                                "program.starttime = programgenres.starttime ");
            }

            where += QString("WHERE channel.deleted IS NULL "
                             "  AND channel.visible > 0 "
                             "  AND program.endtime > :PGILSTART "
                             "  AND ( ") + powerWhere + " ) ";
            MSqlAddMoreBindings(bindings, powerBindings);
        }
    }
    else if (m_type == plSQLSearch) // complex search
    {
        qphrase.remove(RecordingInfo::kReLeadingAnd);
        where = QString("WHERE channel.deleted iS NULL "
                        "  AND channel.visible > 0 "
                        "  AND program.endtime > :PGILSTART "
                        "  AND ( %1 ) ").arg(qphrase);
        if (!m_extraArg.isEmpty())
            where = m_extraArg + ' ' + where;
    }
    else if (m_type == plChannel) // list by channel
    {
        groupBy = ProgGroupBy::None;
        where = "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND channel.chanid = :PGILPHRASE2 ";
        bindings[":PGILPHRASE2"] = qphrase;
    }
    else if (m_type == plCategory) // list by category
    {
        if (!m_useGenres)
        {
            where = "WHERE channel.deleted IS NULL "
                "  AND channel.visible > 0 "
                "  AND program.endtime > :PGILSTART "
                "  AND program.category = :PGILPHRASE3 ";
            bindings[":PGILPHRASE3"] = qphrase;
        }
        else if (m_viewList[m_curView].indexOf(":/:") < 0)
        {
            where = "JOIN programgenres g ON "
                "  program.chanid = g.chanid AND "
                "  program.starttime = g.starttime AND "
                "  genre = :PGILPHRASE4 "
                "WHERE channel.deleted IS NULL "
                "  AND channel.visible > 0 "
                "  AND program.endtime > :PGILSTART ";
            bindings[":PGILPHRASE4"] = qphrase;
        }
        else
        {
            where = "JOIN programgenres g1 ON "
                "  program.chanid = g1.chanid AND "
                "  program.starttime = g1.starttime AND "
                "  g1.genre = :GENRE1 "
                "JOIN programgenres g2 ON "
                "  program.chanid = g2.chanid AND "
                "  program.starttime = g2.starttime AND "
                "  g2.genre = :GENRE2 "
                "WHERE channel.deleted IS NULL "
                "  AND channel.visible > 0 "
                "  AND program.endtime > :PGILSTART ";
            bindings[":GENRE1"] = m_viewList[m_curView].section(":/:", 0, 0);
            bindings[":GENRE2"] = m_viewList[m_curView].section(":/:", 1, 1);
        }
    }
    else if (m_type == plMovies) // list movies
    {
        where = "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND program.category_type = 'movie' "
            "  AND program.stars " + qphrase + ' ';
    }
    else if (m_type == plTime) // list by time
    {
        groupBy = ProgGroupBy::ChanNum;
        QDateTime searchTime(m_searchTime);
        searchTime.setTime(QTime(searchTime.time().hour(), 0, 0));
        bindings[":PGILSEARCHTIME1"] = searchTime;
        where = "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.starttime >= :PGILSEARCHTIME1 ";
        if (m_titleSort)
        {
            where += "  AND program.starttime < DATE_ADD(:PGILSEARCHTIME2, "
                "INTERVAL '1' HOUR) ";
            bindings[":PGILSEARCHTIME2"] = bindings[":PGILSEARCHTIME1"];
        }
    }
    else if (m_type == plRecordid) // list by recordid
    {
        where = "JOIN recordmatch ON "
            " (program.starttime = recordmatch.starttime "
            "  AND program.chanid = recordmatch.chanid) "
            "WHERE channel.deleted IS NULL "
            "  AND channel.visible > 0 "
            "  AND program.endtime > :PGILSTART "
            "  AND recordmatch.recordid = :PGILPHRASE5 ";
        bindings[":PGILPHRASE5"] = qphrase;
    }
    else if (m_type == plStoredSearch) // stored search
    {
        MSqlQuery query(MSqlQuery::InitCon());
        query.prepare("SELECT fromclause, whereclause FROM customexample "
                      "WHERE rulename = :RULENAME;");
        query.bindValue(":RULENAME", qphrase);

        if (query.exec() && query.next())
        {
            QString fromc = query.value(0).toString();
            QString wherec = query.value(1).toString();

            where = QString("WHERE channel.deleted IS NULL "
                            "  AND channel.visible > 0 "
                            "  AND program.endtime > :PGILSTART "
                            "  AND ( %1 ) ").arg(wherec);
            if (!fromc.isEmpty())
                where = fromc + ' ' + where;
        }
    }
    else if (m_type == plPreviouslyRecorded)
    {
        if (m_recid && !m_title.isEmpty())
        {
            where = QString("AND ( recordid = %1 OR title = :MTITLE )")
                .arg(m_recid);
            bindings[":MTITLE"] = m_title;
        }
        else if (!m_title.isEmpty())
        {
            where = QString("AND title = :MTITLE ");
            bindings[":MTITLE"] = m_title;
        }
        else if (m_recid)
        {
            where = QString("AND recordid = %1 ").arg(m_recid);
        }
    }

    ProgramInfo        selected;
    const ProgramInfo *selectedP = (restorePosition) ? GetCurrentProgram() : nullptr;
    if (selectedP)
    {
        selected = *selectedP;
        selectedP = &selected;
    }

    // Save a copy of m_itemList so that deletion of the ProgramInfo
    // objects can be deferred until background processing of old
    // ProgramInfo objects has completed.
    m_itemListSave.clear();
    m_itemListSave = m_itemList;
    m_itemList.setAutoDelete(false);
    m_itemList.clear();
    m_itemList.setAutoDelete(true);

    if (m_type == plPreviouslyRecorded)
    {
        LoadFromOldRecorded(m_itemList, where, bindings);
    }
    else
    {
        LoadFromScheduler(m_schedList);
        LoadFromProgram(m_itemList, where, bindings, m_schedList, groupBy);
    }

    if (m_type == plNewListings || m_titleSort)
    {
        SortList(kTitleSort, m_reverseSort);
        if (m_type != plPreviouslyRecorded)
        {
            // Prune to one per title
            QString curtitle;
            auto it = m_itemList.begin();
            while (it != m_itemList.end())
            {
                if ((*it)->GetSortTitle() != curtitle)
                {
                    curtitle = (*it)->GetSortTitle();
                    ++it;
                }
                else
                {
                    it = m_itemList.erase(it);
                }
            }
        }
    }

    if (!m_titleSort)
        SortList(GetSortBy(), m_reverseSort);

    if (updateDisp)
        UpdateDisplay(selectedP);
}

ProgLister::SortBy ProgLister::GetSortBy(void) const
{
    if (!m_titleSort)
        return kTimeSort;
    if (m_type == plPreviouslyRecorded)
        return kPrevTitleSort;
    return kTitleSort;
}

void ProgLister::SortList(SortBy sortby, bool reverseSort)
{
    if (reverseSort)
    {
        if (kTimeSort == sortby)
        {
            std::stable_sort(m_itemList.rbegin(), m_itemList.rend(), plTimeSort);
        }
        else if (kPrevTitleSort == sortby)
        {
            std::stable_sort(m_itemList.rbegin(), m_itemList.rend(),
                        plPrevTitleSort);
        }
        else
        {
            std::stable_sort(m_itemList.rbegin(), m_itemList.rend(), plTitleSort);
        }
    }
    else
    {
        if (kTimeSort == sortby)
        {
            std::stable_sort(m_itemList.begin(), m_itemList.end(), plTimeSort);
        }
        else if (kPrevTitleSort == sortby)
        {
            std::stable_sort(m_itemList.begin(), m_itemList.end(),plPrevTitleSort);
        }
        else
        {
            std::stable_sort(m_itemList.begin(), m_itemList.end(), plTitleSort);
        }
    }
}

void ProgLister::ClearCurrentProgramInfo(void)
{
    InfoMap infoMap;
    ProgramInfo pginfo;
    pginfo.ToMap(infoMap);
    ResetMap(infoMap);

    if (m_positionText)
        m_positionText->Reset();
}

void ProgLister::UpdateDisplay(const ProgramInfo *selected)
{
    int offset = 0;

    if (selected)
        offset = m_progList->GetCurrentPos() - m_progList->GetTopItemPos();

    m_progList->Reset();

    if (m_messageText)
        m_messageText->SetVisible(m_itemList.empty());

    ClearCurrentProgramInfo();

    if (m_curviewText && m_curView >= 0)
        m_curviewText->SetText(m_viewTextList[m_curView]);

    UpdateButtonList();

    if (selected)
        RestoreSelection(selected, offset);
    else if (m_selectedTime.isValid())
    {
        size_t i = 0;
        for (i = 0; i < m_itemList.size(); ++i)
        {
            if (m_selectedTime <= m_itemList[i]->GetScheduledStartTime())
                break;
        }
        m_progList->SetItemCurrent(i);
        m_selectedTime = QDateTime();
    }
}

void ProgLister::RestoreSelection(const ProgramInfo *selected,
                                  int selectedOffset)
{
    using ProgramInfoSortFn = bool (*)(const ProgramInfo *a, const ProgramInfo *b);
    ProgramInfoSortFn comp { nullptr };
    if (!m_titleSort)
        comp = plTimeSort;
    else if (m_type == plPreviouslyRecorded)
        comp = plPrevTitleSort;
    else
        comp = plTitleSort;

    int i = 0;
    for (i = m_itemList.size() - 2; i >= 0; i--)
    {
        bool dobreak = false;
        if (m_reverseSort)
            dobreak = comp(selected, m_itemList[i]);
        else
            dobreak = comp(m_itemList[i], selected);
        if (dobreak)
            break;
    }

    m_progList->SetItemCurrent(i + 1, i + 1 - selectedOffset);
}

void ProgLister::HandleVisible(MythUIButtonListItem *item)
{
    auto *pginfo = item->GetData().value<ProgramInfo*>();

    if (item->GetText("is_item_initialized").isNull())
    {
        InfoMap infoMap;
        pginfo->ToMap(infoMap);

        QString state = RecStatus::toUIState(pginfo->GetRecordingStatus());
        if ((state == "warning") && (plPreviouslyRecorded == m_type))
            state = "disabled";

        item->SetTextFromMap(infoMap, state);

        if (m_type == plTitle)
        {
            QString tempSubTitle = pginfo->GetSubtitle();
            if (tempSubTitle.trimmed().isEmpty())
                tempSubTitle = pginfo->GetTitle();
            item->SetText(tempSubTitle, "titlesubtitle", state);
        }

        item->DisplayState(QString::number(pginfo->GetStars(10)),
                           "ratingstate");

        item->DisplayState(state, "status");

        // Mark this button list item as initialized.
        item->SetText("yes", "is_item_initialized");
    }
}

void ProgLister::UpdateButtonList(void)
{
    for (auto *it : m_itemList)
        new MythUIButtonListItem(m_progList, "", QVariant::fromValue(it));
    m_progList->LoadInBackground();

    if (m_positionText)
    {
        m_positionText->SetText(
            tr("%1 of %2", "Current position in list where %1 is the "
               "position, %2 is the total count")
            .arg(QLocale::system().toString(m_progList->IsEmpty() ? 0 : m_progList->GetCurrentPos() + 1),
                 QLocale::system().toString(m_progList->GetCount())));
    }
}

void ProgLister::HandleSelected(MythUIButtonListItem *item)
{
    if (!item)
    {
        ClearCurrentProgramInfo();
        return;
    }

    auto *pginfo = item->GetData().value<ProgramInfo*> ();
    if (!pginfo)
    {
        ClearCurrentProgramInfo();
        return;
    }

    InfoMap infoMap;
    pginfo->ToMap(infoMap);
    SetTextFromMap(infoMap);

    if (m_positionText)
    {
        m_positionText->SetText(
            tr("%1 of %2", "Current position in list where %1 is the "
               "position, %2 is the total count")
            .arg(QLocale::system().toString(m_progList->IsEmpty() ? 0 : m_progList->GetCurrentPos() + 1),
                 QLocale::system().toString(m_progList->GetCount())));
    }

    MythUIStateType *ratingState = dynamic_cast<MythUIStateType*>
        (GetChild("ratingstate"));

    if (ratingState)
    {
        QString rating = QString::number(pginfo->GetStars(10));
        ratingState->DisplayState(rating);
    }
}

void ProgLister::customEvent(QEvent *event)
{
    bool needUpdate = false;

    if (event->type() == DialogCompletionEvent::kEventType)
    {
        auto *dce = (DialogCompletionEvent*)(event);

        QString resultid   = dce->GetId();
//      QString resulttext = dce->GetResultText();
        int     buttonnum  = dce->GetResult();

        if (resultid == "sortgroupmenu")
        {
            switch (buttonnum)
            {
                case 0:
                    m_reverseSort = !m_reverseSort;
                    needUpdate    = true;
                    break;
                case 1:
                    m_titleSort   = true;
                    m_reverseSort = false;
                    needUpdate    = true;
                    break;
                case 2:
                    m_titleSort   = false;
                    m_reverseSort = (m_type == plPreviouslyRecorded);
                    needUpdate    = true;
                    break;
                default:
                    ScheduleCommon::customEvent(event);
                    break;
            }
        }
        else if (resultid == "deletemenu")
        {
            switch (buttonnum)
            {
                case 0:
                {
                    ProgramInfo *pi = GetCurrentProgram();
                    if (pi)
                    {
                        RecordingInfo ri(*pi);
                        if (ri.IsDuplicate())
                            ri.ForgetHistory();
                        else
                            ri.SetDupHistory();
                        *pi = ri;
                    }
                    break;
                }
                case 1:
                    ShowDeleteOldEpisodeMenu();
                    break;
                case 2:
                    ShowDeleteOldSeriesMenu();
                    break;
            }
        }
        else if (resultid == "deleterule")
        {
            auto *record = dce->GetData().value<RecordingRule *>();
            if (record && buttonnum > 0 && !record->Delete())
            {
                LOG(VB_GENERAL, LOG_ERR, LOC +
                    "Failed to delete recording rule");
            }
            delete record;
        }
        else
        {
            ScheduleCommon::customEvent(event);
        }
    }
    else if (event->type() == ScreenLoadCompletionEvent::kEventType)
    {
        auto *slce = (ScreenLoadCompletionEvent*)(event);
        QString id = slce->GetId();

        if (id == objectName())
        {
            CloseBusyPopup(); // opened by LoadInBackground()
            UpdateDisplay();

            if (m_curView < 0 && m_type != plPreviouslyRecorded)
                ShowChooseViewMenu();
        }
    }
    else if (event->type() == MythEvent::kMythEventMessage)
    {
        auto *me = dynamic_cast<MythEvent *>(event);
        if (me == nullptr)
            return;

        const QString& message = me->Message();

        if (m_allowViewDialog && message == "CHOOSE_VIEW")
            ShowChooseViewMenu();
        else if (message == "SCHEDULE_CHANGE"
                 || message == "GROUPBY_CHANGE")
            needUpdate = true;
    }

    if (needUpdate)
        FillItemList(true);
}
