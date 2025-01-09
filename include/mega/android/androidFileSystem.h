/**
 * @file mega/android/androidFileSystem.h
 * @brief Android filesystem/directory access
 *
 * (c) 2013-2024 by Mega Limited, Auckland, New Zealand
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

#ifndef ANDROIDFILESYSTEM_H
#define ANDROIDFILESYSTEM_H

#include <mega/filesystem.h>

#include <jni.h>

namespace mega
{

class AndroidFileWrapper
{
public:
    AndroidFileWrapper(const std::string& path);
    ~AndroidFileWrapper();
    AndroidFileWrapper(const AndroidFileWrapper&) = delete;
    AndroidFileWrapper& operator=(const AndroidFileWrapper&) = delete;
    AndroidFileWrapper(AndroidFileWrapper&& other) noexcept;
    AndroidFileWrapper& operator=(AndroidFileWrapper&& other) noexcept;
    bool exists();
    int getFileDescriptor(bool write);
    void close();
    std::string getName();
    std::vector<AndroidFileWrapper> getChildren();
    bool isFolder();
    std::string getPath();

private:
    jobject mAndroidFileObject{nullptr};
    std::string mPath;
    int mFd{-1};
    static constexpr char GET_ANDROID_FILE[] = "getFromUri";
    static constexpr char GET_FILE_DESCRIPTOR[] = "getFileDescriptor";
    static constexpr char IS_FOLDER[] = "isFolder";
    static constexpr char GET_NAME[] = "getName";
    static constexpr char GET_CHILDREN_URIS[] = "getChildrenUris";
};

class MEGA_API AndroidPlatformURIHelper: public PlatformURIHelper
{
public:
    bool isURI(const std::string& path) override;
    std::string getName(const std::string& path) override;

private:
    AndroidPlatformURIHelper();

    ~AndroidPlatformURIHelper() override {}

    static AndroidPlatformURIHelper mPlatformHelper;
    static int mNumInstances;
};

class MEGA_API AndroidFileAccess: public FileAccess
{
public:
    // blocking mode: open for reading, writing or reading and writing.
    // This one really does open the file, and openf(), closef() will have no effect
    // If iteratingDir is supplied, this fopen() call must be for the directory entry being iterated
    // by dopen()/dnext()
    bool fopen(const LocalPath&,
               bool read,
               bool write,
               FSLogging,
               DirAccess* iteratingDir = nullptr,
               bool ignoreAttributes = false,
               bool skipcasecheck = false,
               LocalPath* actualLeafNameIfDifferent = nullptr) override;

    // Close an already open file.
    void fclose() override;

    // absolute position write
    bool fwrite(const byte*, unsigned, m_off_t) override;

    // Stat an already open file.
    bool fstat(m_time_t& modified, m_off_t& size) override;

    // Truncate a file.
    bool ftruncate(m_off_t size = 0) override;

    void updatelocalname(const LocalPath& name, bool force) override;

    AndroidFileAccess(Waiter* w, int defaultfilepermissions = 0600, bool followSymLinks = true);
    virtual ~AndroidFileAccess();

    std::unique_ptr<AndroidFileWrapper> stealFileWrapper();

protected:
    // system-specific raw read/open/close to be provided by platform implementation.   fopen /
    // openf / fread etc are implemented by calling these.
    bool sysread(byte*, unsigned, m_off_t) override;
    bool sysstat(m_time_t*, m_off_t*, FSLogging) override;
    bool sysopen(bool async, FSLogging) override;
    void sysclose() override;

    std::unique_ptr<AndroidFileWrapper> mFileWrapper;
    int fd{-1};
    int mDefaultFilePermissions{0600};
    bool mFollowSymLinks{true};
};

class MEGA_API AndroidDirAccess: public DirAccess
{
public:
    bool dopen(LocalPath* path, FileAccess* f, bool doglob) override;
    bool dnext(LocalPath& path, LocalPath& name, bool followsymlinks, nodetype_t* type) override;

private:
    std::unique_ptr<AndroidFileWrapper> mFileWrapper;
    std::vector<AndroidFileWrapper> mChildren;
    size_t mIndex{0};
};
}

#endif // ANDROIDFILESYSTEM_H
