/* Copyright (C) 2013 Webyog Inc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307, USA

*/


#include "wyFile.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>

#ifdef _WIN32
#include <io.h>
#else
// PORT: POSIX counterparts for the Win32 CRT headers above
#include <unistd.h>
#include <cstdlib>
#endif

wyFile::wyFile()
{
#ifdef _WIN32
	m_hfile = NULL;
#else
	// PORT: Linux uses a plain fd; -1 means "closed" (NULL on Windows)
	m_hfile = -1;
#endif
}

wyFile::~wyFile()
{
	Close();
}

void wyFile::SetFilename(wyString * pFileName) 
{
	m_filename.SetAs(pFileName->GetString());
}


wyBool
wyFile::GetTempFilePath(wyString *pTempPath)
{
#ifdef _WIN32
	wyWChar path[MAX_PATH + 1];
			
	if(GetTempPath(MAX_PATH, path) == 0)
		return wyFalse;
	
	/*if(GetLongPathName(path, longpath, MAX_PATH) == 0) 
	{
		return wyFalse;
	}*/

	pTempPath->SetAs(path);
	return wyTrue;	
#else
	// PORT: Win32 GetTempPath() -> $TMPDIR, falling back to /tmp
	const char *tempdir = std::getenv("TMPDIR");
	pTempPath->SetAs((tempdir && *tempdir) ? tempdir : "/tmp");
	return wyTrue;
#endif
}

wyInt32
wyFile::OpenWithPermission(wyInt32 pAccessMode, wyInt32 pCreationDisposition,wyBool inheritable)
{	
	wyInt32 ret = 0;
#ifdef _WIN32
	m_hfile = CreateFile((LPCWSTR)m_filename.GetAsWideChar(), 
						 pAccessMode, 0, NULL, pCreationDisposition, 
						 FILE_ATTRIBUTE_NORMAL, NULL);

	if(m_hfile == INVALID_HANDLE_VALUE) {
		m_hfile = NULL;
		ret = -1;
	}
	else if(inheritable)
	{
	//child process(plink.exe) shoudn't inherit this file.
		if(!SetHandleInformation(m_hfile, HANDLE_FLAG_INHERIT, 0))
		{
		//
		}
	}
	return ret;
#else
	// PORT: callers pass Win32 constants (GENERIC_*, CREATE_*) which
	// port/shim/CommonHelper.h maps onto POSIX open() flags.
	m_hfile = open(m_filename.GetString(), pAccessMode | pCreationDisposition, 0644);

	if(m_hfile == -1) {
		ret = -1;
	}
	else if(inheritable)
	{
		// same intent as above: keep handles away from child processes
		fcntl(m_hfile, F_SETFD, FD_CLOEXEC);
	}
	return (ret == 0) ? m_hfile : ret;
#endif
}

wyInt32 
wyFile::Close()
{
	wyInt32 ret = 0;
#ifdef _WIN32
	if (m_hfile == NULL) {
		return ret;
	}
	ret = CloseHandle(m_hfile);
	m_hfile = NULL;
#else
	// PORT: close(fd); -1 sentinel mirrors the NULL check above
	if (m_hfile == -1) {
		return ret;
	}
	ret = close(m_hfile);
	m_hfile = -1;
#endif
	return ret;
}

wyBool
wyFile::CheckIfFileExists()
{
#ifdef _WIN32
	WIN32_FIND_DATA FindFileData;
	HANDLE hFind;

	if (m_filename.GetLength() == 0) 
	{
		return wyFalse;
	}

	hFind = FindFirstFile(m_filename.GetAsWideChar(), &FindFileData);
	if (hFind == INVALID_HANDLE_VALUE) 
	{
		return wyFalse;
	} 
	else 
	{
		FindClose(hFind);
		return wyTrue;
	}
#else
	// PORT: Win32 FindFirstFile probe -> stat()
	if (m_filename.GetLength() == 0)
	{
		return wyFalse;
	}

	struct stat st;
	return stat(m_filename.GetString(), &st) == 0 ? wyTrue : wyFalse;
#endif
}

wyInt32 
wyFile::RemoveFile()
{
	wyInt32 ret = 0;

	if(m_filename.GetLength())
	{
#ifdef _WIN32
		ret = DeleteFile(m_filename.GetAsWideChar());
#else
		// PORT: DeleteFile() nonzero-on-success -> unlink() mapped to match
		ret = (unlink(m_filename.GetString()) == 0) ? 1 : 0;
#endif
	}

	return ret;
}

// PORT: phantom member referenced by wySqlite.cpp's Linux path; implemented
// to match the comment there ("new DB -> set the permission accordingly").
wyBool
wyFile::SetFilePermission(const wyChar *path)
{
#ifdef _WIN32
	(void)path;	// call site is #ifndef _WIN32 only
	return wyTrue;
#else
	return chmod(path, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH) == 0 ? wyTrue : wyFalse;
#endif
}
