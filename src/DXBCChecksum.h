#pragma once
#ifndef DXBCCHECKSUM_H
#define DXBCCHECKSUM_H

#include <windows.h>

/// Calculate the DXBC checksum for the provided chunk of memory.
/// \param[in] pData    A pointer to the memory to checksum.
/// \param[in] dwSize   The size of the memory to checksum.
/// \param[out] dwHash  A DWORD array to copy the calculated check sum into.
/// \return TRUE if successful, otherwise FALSE.
BOOL CalculateDXBCChecksum(BYTE* pData, DWORD dwSize, DWORD dwHash[4]);

#endif // DXBCCHECKSUM_H
