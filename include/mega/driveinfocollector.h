/**
 * @file mega/driveinfocollector.h
 * @brief Mega SDK various utilities and helper classes
 *
 * (c) 2013-2020 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#pragma once

#ifdef USE_DRIVE_NOTIFICATIONS

#include "mega/drivenotify.h"

#include <functional>
#include <string>
#include <queue>
#include <mutex>

namespace mega {

    class DriveInfoCollector
    {
    public:
        bool start(std::function<void()> notify);
        void stop();

        std::pair<std::wstring, bool> get();

        ~DriveInfoCollector() { stop(); }

    private:
        DriveNotify mNotifier;
        std::queue<DriveInfo> mInfoQueue;
        std::mutex mSyncAccessMutex;

        std::function<void()> mNotifyOnInfo;

        void add(DriveInfo&& info);
    };

} // namespace mega

#endif // USE_DRIVE_NOTIFICATIONS
