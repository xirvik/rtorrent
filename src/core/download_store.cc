// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2005-2011, Jari Sundell <jaris@ifi.uio.no>

// DownloadStore handles the saving and listing of session torrents.

#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

#include <torrent/exceptions.h>
#include <torrent/object.h>
#include <torrent/object_stream.h>
#include <torrent/rate.h>
#include <torrent/torrent.h>
#include <torrent/utils/error_number.h>
#include <torrent/utils/log.h>
#include <torrent/utils/path.h>
#include <torrent/utils/resume.h>
#include <torrent/utils/string_manip.h>

#include "utils/directory.h"

#include "core/download.h"
#include "core/download_store.h"

namespace core {

void
DownloadStore::enable(bool lock) {
  if (is_enabled())
    throw torrent::input_error("Session directory already enabled.");

  if (m_path.empty())
    return;

  if (lock)
    m_lockfile.set_path(m_path + "rtorrent.lock");
  else
    m_lockfile.set_path(std::string());

  if (!m_lockfile.try_lock()) {
    if (torrent::utils::error_number::current().is_bad_path()) {
      throw torrent::input_error(
        "Could not lock session directory: \"" + m_path + "\", " +
        torrent::utils::error_number::current().message());
    } else {
      auto msg = "Could not lock session directory: \"" + m_path +
                 "\", held by \"" + m_lockfile.locked_by_as_string() + "\".";

      if (::getpid() < 10) {
        // Hint for container users
        msg += '\n';
        msg += "Hint: use a consistent hostname so rTorrent can safely handle "
               "stale locks.";
      }

      throw torrent::input_error(msg);
    }
  }
}

void
DownloadStore::disable() {
  if (!is_enabled())
    return;

  m_lockfile.unlock();
}

void
DownloadStore::set_path(const std::string& path) {
  if (is_enabled())
    throw torrent::input_error(
      "Tried to change session directory while it is enabled.");

  if (!path.empty() && *path.rbegin() != '/')
    m_path = torrent::utils::path_expand(path + '/');
  else
    m_path = torrent::utils::path_expand(path);
}

namespace {

bool
sync_session_directory(const std::string& path, bool whole_filesystem) {
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

  if (fd < 0)
    return false;

  int result = whole_filesystem ? ::syncfs(fd) : ::fsync(fd);

  if (::close(fd) == -1)
    return false;

  return result == 0;
}

}

bool
DownloadStore::write_bencode(const std::string&     filename,
                             const torrent::Object& obj,
                             uint32_t               skip_mask,
                             bool                   sync) {
  torrent::Object tmp;
  std::fstream    output(filename.c_str(), std::ios::out | std::ios::trunc);
  int fd = -1;

  if (!output.is_open())
    goto download_store_save_error;

  torrent::object_write_bencode(&output, &obj, skip_mask);

  if (!output.good())
    goto download_store_save_error;

  // The data only reaches the kernel here, so this is where a full disk is seen.
  output.close();

  if (output.fail())
    return false;

  // Test the new file, to ensure it is a valid bencode string.
  output.open(filename.c_str(), std::ios::in);
  output >> tmp;

  if (!output.good())
    goto download_store_save_error;

  output.close();

  if (!sync)
    return true;

  // Ensure that the new file is actually written to the disk
  fd = ::open(filename.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    goto download_store_save_error;

  if (::fsync(fd) == -1) {
    ::close(fd);
    return false;
  }

  return ::close(fd) == 0;

download_store_save_error:
  output.close();
  return false;
}

std::string
DownloadStore::prepare_save(Download* d) {
  torrent::Object* resume_base =
    &d->download()->bencode()->get_key("libtorrent_resume");
  torrent::Object* rtorrent_base =
    &d->download()->bencode()->get_key("rtorrent");

  // Move this somewhere else?
  rtorrent_base->insert_key("chunks_done",
                            d->download()->file_list()->completed_chunks());
  rtorrent_base->insert_key("chunks_wanted",
                            d->download()->data()->wanted_chunks());
  rtorrent_base->insert_key("total_uploaded", d->info()->up_rate()->total());
  rtorrent_base->insert_key("total_downloaded",
                            d->info()->down_rate()->total());

  // Don't save for completed torrents when we've cleared the uncertain_pieces.
  torrent::resume_save_progress(*d->download(), *resume_base);
  torrent::resume_save_uncertain_pieces(*d->download(), *resume_base);

  torrent::resume_save_addresses(*d->download(), *resume_base);
  torrent::resume_save_file_priorities(*d->download(), *resume_base);
  torrent::resume_save_tracker_settings(*d->download(), *resume_base);

  // Temp fixing of all flags, move to a better place:
  resume_base->set_flags(torrent::Object::flag_session_data);
  rtorrent_base->set_flags(torrent::Object::flag_session_data);

  return create_filename(d);
}

bool
DownloadStore::write_resume_files(Download*          d,
                                  const std::string& base_filename,
                                  bool               sync) {
  return write_bencode(base_filename + ".libtorrent_resume.new",
                       d->download()->bencode()->get_key("libtorrent_resume"),
                       0,
                       sync) &&
         write_bencode(base_filename + ".rtorrent.new",
                       d->download()->bencode()->get_key("rtorrent"),
                       0,
                       sync);
}

bool
DownloadStore::rename_resume_files(const std::string& base_filename) {
  return ::rename((base_filename + ".libtorrent_resume.new").c_str(),
                  (base_filename + ".libtorrent_resume").c_str()) == 0 &&
         ::rename((base_filename + ".rtorrent.new").c_str(),
                  (base_filename + ".rtorrent").c_str()) == 0;
}

bool
DownloadStore::save(Download* d, int flags) {
  if (!is_enabled())
    return true;

  std::string base_filename = prepare_save(d);

  if (!write_resume_files(d, base_filename, true) ||
      !rename_resume_files(base_filename))
    return false;

  if (!(flags & flag_skip_static) &&
      write_bencode(base_filename + ".new",
                    *d->bencode(),
                    torrent::Object::flag_session_data,
                    true))
    ::rename((base_filename + ".new").c_str(), base_filename.c_str());

  sync_session_directory(m_path, false);

  return true;
}

// The periodic save rewrites the resume files of every download. Syncing each
// file costs a journal commit per file, on the main thread, so a large session
// would block the client for minutes. Write all the files first, then make
// them durable with one syncfs() before any rename, so a crash still leaves
// each download with either its old or its new complete files, and commit the
// renames with one fsync() of the session directory.
unsigned int
DownloadStore::save_resume_all(const std::vector<Download*>& downloads) {
  if (!is_enabled())
    return downloads.size();

  std::vector<std::string> written;
  written.reserve(downloads.size());

  for (auto d : downloads) {
    std::string base_filename = prepare_save(d);

    if (write_resume_files(d, base_filename, false))
      written.push_back(std::move(base_filename));
  }

  if (written.empty())
    return 0;

  if (!sync_session_directory(m_path, true)) {
    lt_log_print(torrent::LOG_ERROR,
                 "Could not sync the session directory \"%s\" to disk, the "
                 "session files were not replaced: %s",
                 m_path.c_str(),
                 torrent::utils::error_number::current().message().c_str());
    return 0;
  }

  unsigned int saved = 0;

  for (const auto& base_filename : written)
    if (rename_resume_files(base_filename))
      saved++;

  if (!sync_session_directory(m_path, false))
    lt_log_print(torrent::LOG_ERROR,
                 "Could not sync the session directory \"%s\" after "
                 "replacing the session files: %s",
                 m_path.c_str(),
                 torrent::utils::error_number::current().message().c_str());

  return saved;
}

void
DownloadStore::remove(Download* d) {
  if (!is_enabled())
    return;

  ::unlink((create_filename(d) + ".libtorrent_resume").c_str());
  ::unlink((create_filename(d) + ".rtorrent").c_str());
  ::unlink(create_filename(d).c_str());
}

// This also needs to check that it isn't a directory.
bool
not_correct_format(const utils::directory_entry& entry) {
  return !DownloadStore::is_correct_format(entry.d_name);
}

utils::Directory
DownloadStore::get_formated_entries() {
  if (!is_enabled())
    return utils::Directory();

  utils::Directory d(m_path);

  if (!d.update(utils::Directory::update_hide_dot))
    throw torrent::storage_error(
      "core::DownloadStore::update() could not open directory \"" + m_path +
      "\"");

  d.erase(std::remove_if(d.begin(),
                         d.end(),
                         [](const utils::directory_entry& entry) {
                           return not_correct_format(entry);
                         }),
          d.end());

  return d;
}

bool
DownloadStore::is_correct_format(const std::string& f) {
  if (f.size() != 48 || f.substr(40) != ".torrent")
    return false;

  for (std::string::const_iterator itr = f.begin(); itr != f.end() - 8; ++itr)
    if (!(*itr >= '0' && *itr <= '9') && !(*itr >= 'A' && *itr <= 'F'))
      return false;

  return true;
}

std::string
DownloadStore::create_filename(Download* d) {
  return m_path +
         torrent::utils::transform_hex(d->info()->hash().begin(),
                                       d->info()->hash().end()) +
         ".torrent";
}

}
