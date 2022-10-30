
#include "lookup.h"

#include <vector>

#include <QList>

#include "programinfo.h"
#include "recordingrule.h"
#include "mythlogging.h"
#include "jobqueue.h"
#include "remoteutil.h"
#include "mythcorecontext.h"

#include "metadataimagehelper.h"

LookerUpper::LookerUpper()
{
    m_metadataFactory = new MetadataFactory(this);
}

LookerUpper::~LookerUpper()
{
    while (!m_busyRecList.isEmpty())
        delete m_busyRecList.takeFirst();
}

bool LookerUpper::StillWorking()
{
    return m_metadataFactory->IsRunning() ||
        (m_busyRecList.count() != 0);
}

void LookerUpper::HandleSingleRecording(const uint chanid,
                                        const QDateTime &starttime,
                                        bool updaterules)
{
    auto *pginfo = new ProgramInfo(chanid, starttime);

    if (!pginfo)
    {
        LOG(VB_GENERAL, LOG_ERR,
            "No valid program info for supplied chanid/starttime");
        return;
    }

    m_updaterules = updaterules;
    m_updateartwork = true;

    m_busyRecList.append(pginfo);
    m_metadataFactory->Lookup(pginfo, true, m_updateartwork, false);
}

void LookerUpper::HandleAllRecordings(bool updaterules)
{
    QMap< QString, ProgramInfo* > recMap;
    QMap< QString, uint32_t > inUseMap = ProgramInfo::QueryInUseMap();
    QMap< QString, bool > isJobRunning = ProgramInfo::QueryJobsRunning(JOB_COMMFLAG);

    m_updaterules = updaterules;

    ProgramList progList;

    LoadFromRecorded( progList, false, inUseMap, isJobRunning, recMap, -1 );

    for (auto *pg : progList)
    {
        auto *pginfo = new ProgramInfo(*pg);
        if ((pginfo->GetRecordingGroup() != "Deleted") &&
            (pginfo->GetRecordingGroup() != "LiveTV") &&
            (pginfo->GetInetRef().isEmpty() ||
            (!pginfo->GetSubtitle().isEmpty() &&
            (pginfo->GetSeason() == 0) &&
            (pginfo->GetEpisode() == 0))))
        {
            QString msg = QString("Looking up: %1 %2")
                .arg(pginfo->GetTitle(), pginfo->GetSubtitle());
            LOG(VB_GENERAL, LOG_INFO, msg);

            m_busyRecList.append(pginfo);
            m_metadataFactory->Lookup(pginfo, true, false, false);
        }
        else
            delete pginfo;
    }
}

void LookerUpper::HandleAllRecordingRules()
{
    m_updaterules = true;

    std::vector<ProgramInfo *> recordingList;

    RemoteGetAllScheduledRecordings(recordingList);

    for (auto & pg : recordingList)
    {
        auto *pginfo = new ProgramInfo(*pg);
        if (pginfo->GetInetRef().isEmpty())
        {
            QString msg = QString("Looking up: %1 %2")
                .arg(pginfo->GetTitle(), pginfo->GetSubtitle());
            LOG(VB_GENERAL, LOG_INFO, msg);

            m_busyRecList.append(pginfo);
            m_metadataFactory->Lookup(pginfo, true, false, true);
        }
        else
            delete pginfo;
    }
}

void LookerUpper::HandleAllArtwork(bool aggressive)
{
    m_updateartwork = true;

    if (aggressive)
        m_updaterules = true;

    // First, handle all recording rules w/ inetrefs
    std::vector<ProgramInfo *> recordingList;

    RemoteGetAllScheduledRecordings(recordingList);
    int maxartnum = 3;

    for (auto & pg : recordingList)
    {
        auto *pginfo = new ProgramInfo(*pg);
        bool dolookup = true;

        if (pginfo->GetInetRef().isEmpty())
            dolookup = false;
        if (dolookup || aggressive)
        {
            ArtworkMap map = GetArtwork(pginfo->GetInetRef(), pginfo->GetSeason(), true);
            if (map.isEmpty() || (aggressive && map.count() < maxartnum))
            {
                QString msg = QString("Looking up artwork for recording rule: %1 %2")
                    .arg(pginfo->GetTitle(), pginfo->GetSubtitle());
                LOG(VB_GENERAL, LOG_INFO, msg);

                m_busyRecList.append(pginfo);
                m_metadataFactory->Lookup(pginfo, true, true, true);
                continue;
            }
        }
        delete pginfo;
    }

    // Now, Attempt to fill in the gaps for recordings
    QMap< QString, ProgramInfo* > recMap;
    QMap< QString, uint32_t > inUseMap = ProgramInfo::QueryInUseMap();
    QMap< QString, bool > isJobRunning = ProgramInfo::QueryJobsRunning(JOB_COMMFLAG);

    ProgramList progList;

    LoadFromRecorded( progList, false, inUseMap, isJobRunning, recMap, -1 );

    for (auto *pg : progList)
    {
        auto *pginfo = new ProgramInfo(*pg);

        bool dolookup = true;

        LookupType type = GuessLookupType(pginfo);

        if (type == kProbableMovie)
           maxartnum = 2;

        if ((!aggressive && type == kProbableGenericTelevision) ||
             pginfo->GetRecordingGroup() == "Deleted" ||
             pginfo->GetRecordingGroup() == "LiveTV")
            dolookup = false;
        if (dolookup || aggressive)
        {
            ArtworkMap map = GetArtwork(pginfo->GetInetRef(), pginfo->GetSeason(), true);
            if (map.isEmpty() || (aggressive && map.count() < maxartnum))
            {
               QString msg = QString("Looking up artwork for recording: %1 %2")
                   .arg(pginfo->GetTitle(), pginfo->GetSubtitle());
                LOG(VB_GENERAL, LOG_INFO, msg);

                m_busyRecList.append(pginfo);
                m_metadataFactory->Lookup(pginfo, true, true, aggressive);
                continue;
            }
        }
        delete pginfo;
    }

}

void LookerUpper::CopyRuleInetrefsToRecordings()
{
    QMap< QString, ProgramInfo* > recMap;
    QMap< QString, uint32_t > inUseMap = ProgramInfo::QueryInUseMap();
    QMap< QString, bool > isJobRunning = ProgramInfo::QueryJobsRunning(JOB_COMMFLAG);

    ProgramList progList;

    LoadFromRecorded( progList, false, inUseMap, isJobRunning, recMap, -1 );

    for (auto *pg : progList)
    {
        auto *pginfo = new ProgramInfo(*pg);
        if (pginfo && pginfo->GetInetRef().isEmpty())
        {
            auto *rule = new RecordingRule();
            rule->m_recordID = pginfo->GetRecordingRuleID();
            rule->Load();
            if (!rule->m_inetref.isEmpty())
            {
                QString msg = QString("%1").arg(pginfo->GetTitle());
                if (!pginfo->GetSubtitle().isEmpty())
                    msg += QString(": %1").arg(pginfo->GetSubtitle());
                msg += " has no inetref, but its recording rule does. Copying...";
                LOG(VB_GENERAL, LOG_INFO, msg);
                pginfo->SaveInetRef(rule->m_inetref);
            }
            delete rule;
        }
        delete pginfo;
    }
}

void LookerUpper::customEvent(QEvent *levent)
{
    QString manRecSuffix = QString(" (%1)").arg(QObject::tr("Manual Record"));

    if (levent->type() == MetadataFactoryMultiResult::kEventType)
    {
        auto *mfmr = dynamic_cast<MetadataFactoryMultiResult*>(levent);

        if (!mfmr)
            return;

        MetadataLookupList list = mfmr->m_results;

        if (list.count() > 1)
        {
            int yearindex = -1;
            MetadataLookup *bestTitleMeta = nullptr;
            QDate bestTitleDate;
            float bestTitlePopularity = 0.0;
            unsigned int bestScoreSoFar = 0;

            for (int p = 0; p != list.size(); ++p)
            {
                // Calculate a score based on an exact title match, a matching language,
                // availability of artwork, popularity, and release date.
                unsigned int score = 0;

                auto *pginfo = list[p]->GetData().value<ProgramInfo *>();
                QString title = pginfo->GetTitle();
                title.replace(manRecSuffix,"");

                float match_quality = getStringMatchQuality(title.toLower(),
                                          list[p]->GetBaseTitle().toLower());

                if (match_quality == 1.0F)
                {
                    score = 0x40; // exact title match
                }

                if (match_quality >= 0.7F)
                {
                    score |= 0x20; // close (70%) title match
                }

                if (QString::compare(gCoreContext->GetLanguage(), list[p]->GetLanguage()) == 0)
                {
                    score |= 0x10;  // matching language
                }

                if (!(list[p]->GetArtwork(kArtworkFanart)).empty())
                {
                    score |= 0x08;  // background artwork available
                }

                if (!(list[p]->GetArtwork(kArtworkCoverart)).empty())
                {
                    score |= 0x04;  // cover artwork available
                }

                // Compare popularity to the best match so far
                if (list[p]->GetPopularity() > bestTitlePopularity)
                {
                    score |= 0x02;  // more popular
                }

                // Compare release date to the best match so far
                if (list[p]->GetReleaseDate() > bestTitleDate)
                {
                    score |= 0x01;  // more recent
                }

                if (score >= bestScoreSoFar)
                {
                    // remember the best match, so far
                    bestTitleMeta = list[p];   // set the best match, so far
                    bestTitleDate = bestTitleMeta->GetReleaseDate();
                    bestTitlePopularity = bestTitleMeta->GetPopularity();
                    bestScoreSoFar = score;
                }

                LOG(VB_GENERAL, LOG_INFO,
                    QString("Comparing metadata title '%1' [%2] to recording title '%3', score=0x%4")
                    .arg(title, list[p]->GetReleaseDate().toString(), list[p]->GetBaseTitle())
                    .arg(score, 2, 16));

                if (pginfo && !pginfo->GetSeriesID().isEmpty() &&
                    pginfo->GetSeriesID() == (list[p])->GetTMSref())
                {
                    MetadataLookup *lookup = list[p];
                    if (lookup->GetSubtype() != kProbableGenericTelevision)
                    {
                        if (((pginfo->GetSeason() == 0) && lookup->GetSeason()) ||
                            ((pginfo->GetEpisode() == 0) && lookup->GetEpisode()))
                        {
                            pginfo->SaveSeasonEpisode(lookup->GetSeason(), lookup->GetEpisode());
                        }
                        if (pginfo->GetSubtitle().isEmpty() && !lookup->GetSubtitle().isEmpty())
                        {
                            pginfo->SaveSubtitle(lookup->GetSubtitle());
                        }
                        if (list[p]->GetTitle().contains(manRecSuffix))
                        {
                            // It was a Manual Record rule.
                            // For such recordings, the 'description' field may be populated
                            // with a Timestamp, from Scheduler::UpdateManuals(uint recordid),
                            // or with the Title, possibly from ProgramInfo::ProgramInfo().
                            // Neither a Timestamp nor a Title are of value in a Description
                            // field, so we want to overwrite them with a real description.
                            pginfo->SaveDescription(lookup->GetDescription());
                        }
                        else
                        {
                            // A Program Guide record rule.
                            // These recordings may have already collected a specific
                            // description for the episode. We don't want to overwrite
                            // the specific episode description with a generic description
                            // which the lookup service might provide for the series. So,
                            // only save the lookup description if the current description
                            // is empty at this point.
                            if (pginfo->GetDescription().isEmpty() && !lookup->GetDescription().isEmpty())
                            {
                                pginfo->SaveDescription(lookup->GetDescription());
                            }
                        }
                    }
                    pginfo->SaveInetRef(lookup->GetInetref());
                    m_busyRecList.removeAll(pginfo);
                    return;
                }
                if (pginfo && pginfo->GetYearOfInitialRelease() != 0 &&
                         (list[p])->GetYear() != 0 &&
                         pginfo->GetYearOfInitialRelease() == (list[p])->GetYear())
                {
                    if (yearindex != -1)
                    {
                        LOG(VB_GENERAL, LOG_INFO, "Multiple results matched on year. No definite "
                                      "match could be found.");
                        m_busyRecList.removeAll(pginfo);
                        return;
                    }
                    LOG(VB_GENERAL, LOG_INFO, "Matched from multiple results based on year.");
                    yearindex = p;
                }
            }

            if (yearindex > -1)
            {
                MetadataLookup *lookup = list[yearindex];
                auto *pginfo = lookup->GetData().value<ProgramInfo *>();
                if (lookup->GetSubtype() != kProbableGenericTelevision)
                {
                    if (((pginfo->GetSeason() == 0) && lookup->GetSeason()) ||
                        ((pginfo->GetEpisode() == 0) && lookup->GetEpisode()))
                    {
                        pginfo->SaveSeasonEpisode(lookup->GetSeason(), lookup->GetEpisode());
                    }
                    if (pginfo->GetSubtitle().isEmpty() && !lookup->GetSubtitle().isEmpty())
                    {
                        pginfo->SaveSubtitle(lookup->GetSubtitle());
                    }
                    if (pginfo->GetTitle().contains(manRecSuffix))
                    {
                        // It was a Manual Record rule.
                        // For such recordings, the 'description' field may be populated
                        // with a Timestamp, from Scheduler::UpdateManuals(uint recordid),
                        // or with the Title, possibly from ProgramInfo::ProgramInfo().
                        // Neither a Timestamp nor a Title are of value in a Description
                        // field, so we want to overwrite them with a real description.
                        pginfo->SaveDescription(lookup->GetDescription());
                    }
                    else
                    {
                        // A Program Guide record rule.
                        // These recordings may have already collected a specific
                        // description for the episode. We don't want to overwrite
                        // the specific episode description with a generic description
                        // which the lookup service might provide for the series. So,
                        // only save the lookup description if the current description
                        // is empty at this point.
                        if (pginfo->GetDescription().isEmpty() && !lookup->GetDescription().isEmpty())
                        {
                            pginfo->SaveDescription(lookup->GetDescription());
                        }
                    }
                }
                pginfo->SaveInetRef(lookup->GetInetref());
                m_busyRecList.removeAll(pginfo);
                return;
            }

            if (bestTitleMeta != nullptr)
            {
                LOG(VB_GENERAL, LOG_INFO, QString("Best match released %1").arg(bestTitleDate.toString()));
                MetadataLookup *lookup = bestTitleMeta;
                auto *pginfo = bestTitleMeta->GetData().value<ProgramInfo *>();
                if (lookup->GetSubtype() != kProbableGenericTelevision)
                {
                    if (((pginfo->GetSeason() == 0) && lookup->GetSeason()) ||
                        ((pginfo->GetEpisode() == 0) && lookup->GetEpisode()))
                    {
                        pginfo->SaveSeasonEpisode(lookup->GetSeason(), lookup->GetEpisode());
                    }
                    if (pginfo->GetSubtitle().isEmpty() && !lookup->GetSubtitle().isEmpty())
                    {
                        pginfo->SaveSubtitle(lookup->GetSubtitle());
                    }
                    if (pginfo->GetTitle().contains(manRecSuffix))
                    {
                        // It was a Manual Record rule.
                        // For such recordings, the 'description' field may be populated
                        // with a Timestamp, from Scheduler::UpdateManuals(uint recordid),
                        // or with the Title, possibly from ProgramInfo::ProgramInfo().
                        // Neither a Timestamp nor a Title are of value in a Description
                        // field, so we want to overwrite them with a real description.
                        pginfo->SaveDescription(lookup->GetDescription());
                    }
                    else
                    {
                        // A Program Guide record rule.
                        // These recordings may have already collected a specific
                        // description for the episode. We don't want to overwrite
                        // the specific episode description with a generic description
                        // which the lookup service might provide for the series. So,
                        // only save the lookup description if the current description
                        // is empty at this point.
                        if (pginfo->GetDescription().isEmpty() && !lookup->GetDescription().isEmpty())
                        {
                            pginfo->SaveDescription(lookup->GetDescription());
                        }
                    }
                }
                pginfo->SaveInetRef(lookup->GetInetref());
                m_busyRecList.removeAll(pginfo);
                return;
            }

            LOG(VB_GENERAL, LOG_INFO, "Unable to match this title, too many possible matches. "
                                      "You may wish to manually set the season, episode, and "
                                      "inetref in the 'Watch Recordings' screen.");

            auto *pginfo = list[0]->GetData().value<ProgramInfo *>();

            if (pginfo)
            {
                m_busyRecList.removeAll(pginfo);
            }
        }
    }
    else if (levent->type() == MetadataFactorySingleResult::kEventType)
    {
        auto *mfsr = dynamic_cast<MetadataFactorySingleResult*>(levent);

        if (!mfsr)
            return;

        MetadataLookup *lookup = mfsr->m_result;

        if (!lookup)
            return;

        auto *pginfo = lookup->GetData().value<ProgramInfo *>();

        // This null check could hang us as this pginfo would then never be
        // removed
        if (!pginfo)
            return;

        LOG(VB_GENERAL, LOG_DEBUG, "I found the following data:");
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Input Title: %1").arg(pginfo->GetTitle()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Input Sub:   %1").arg(pginfo->GetSubtitle()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Title:       %1").arg(lookup->GetTitle()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Subtitle:    %1").arg(lookup->GetSubtitle()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Season:      %1").arg(lookup->GetSeason()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Episode:     %1").arg(lookup->GetEpisode()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        Inetref:     %1").arg(lookup->GetInetref()));
        LOG(VB_GENERAL, LOG_DEBUG,
            QString("        User Rating: %1").arg(lookup->GetUserRating()));

        if (lookup->GetSubtype() != kProbableGenericTelevision)
        {
            if (((pginfo->GetSeason() == 0) && lookup->GetSeason()) ||
                ((pginfo->GetEpisode() == 0) && lookup->GetEpisode()))
            {
                pginfo->SaveSeasonEpisode(lookup->GetSeason(), lookup->GetEpisode());
            }
            if (pginfo->GetSubtitle().isEmpty() && !lookup->GetSubtitle().isEmpty())
            {
                pginfo->SaveSubtitle(lookup->GetSubtitle());
            }
            if (pginfo->GetTitle().contains(manRecSuffix))
            {
                // It was a Manual Record rule.
                // For such recordings, the 'description' field may be populated
                // with a Timestamp, from Scheduler::UpdateManuals(uint recordid),
                // or with the Title, possibly from ProgramInfo::ProgramInfo().
                // Neither a Timestamp nor a Title are of value in a Description
                // field, so we want to overwrite them with a real description.
                pginfo->SaveDescription(lookup->GetDescription());
            }
            else
            {
                // A Program Guide record rule.
                // These recordings may have already collected a specific
                // description for the episode. We don't want to overwrite
                // the specific episode description with a generic description
                // which the lookup service might provide for the series. So,
                // only save the lookup description if the current description
                // is empty at this point.
                if (pginfo->GetDescription().isEmpty() && !lookup->GetDescription().isEmpty())
                {
                    pginfo->SaveDescription(lookup->GetDescription());
                }
            }
        }
        pginfo->SaveInetRef(lookup->GetInetref());

        if (m_updaterules)
        {
            auto *rule = new RecordingRule();
            if (rule)
            {
                rule->LoadByProgram(pginfo);
                if (rule->m_inetref.isEmpty() &&
                    (rule->m_searchType == kNoSearch))
                {
                    rule->m_inetref = lookup->GetInetref();
                }
                rule->m_season = lookup->GetSeason();
                rule->m_episode = lookup->GetEpisode();
                rule->Save();

                delete rule;
            }
        }

        if (m_updateartwork)
        {
            DownloadMap dlmap = lookup->GetDownloads();
            // Convert from QMap to QMultiMap
            ArtworkMap artmap;
            for (auto it = dlmap.cbegin(); it != dlmap.cend(); it++)
                artmap.insert(it.key(), it.value());
            SetArtwork(lookup->GetInetref(),
                       lookup->GetIsCollection() ? 0 : lookup->GetSeason(),
                       gCoreContext->GetMasterHostName(), artmap);
        }

        m_busyRecList.removeAll(pginfo);
    }
    else if (levent->type() == MetadataFactoryNoResult::kEventType)
    {
        auto *mfnr = dynamic_cast<MetadataFactoryNoResult*>(levent);

        if (!mfnr)
            return;

        MetadataLookup *lookup = mfnr->m_result;

        if (!lookup)
            return;

        auto *pginfo = lookup->GetData().value<ProgramInfo *>();

        // This null check could hang us as this pginfo would then never be removed
        if (!pginfo)
            return;

        m_busyRecList.removeAll(pginfo);
    }
}
