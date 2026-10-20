// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

// [Nextendo] The NSA (NetworkServiceAccountId) the account presents on a real Switch: its
// baasUserID, 16 hexadecimal digits. It is the identity the Switch's friends are known by (the
// friendId of nx-account), and the one the account server hands out in the "nsa" field of
// /api/me and /api/friends.
//
// Why it matters for Animal Crossing: New Horizons: the game compares the NSA each player
// announces (the Pia session, the header of a card) with the console's nn::friends list
// (Friend::GetAccountId). A Switch announces its baasUserID and sorts its friends by baasUserID;
// an emulator announcing the PID was never recognised as a friend by a Switch (best friends
// impossible, cards discarded). In ACNH, DEN therefore announces the baasUserID and sorts the
// friends by baasUserID, exactly like a Switch.
//
// Kept on disk per account (nextendo_nsa.txt) so an ACNH launched offline stays consistent.
// Unknown (old account server, unreachable): 0, and the emulator keeps the PID everywhere.
// NEXTENDO_NSA_BAAS=0 turns the whole thing off.
namespace WebService::NextendoNsa {

// Animal Crossing: New Horizons.
constexpr u64 AcnhProgramId = 0x01006F8002326000ULL;

// True for the games that compare the announced NSA with nn::friends and must therefore see the
// same NSA as a real Switch.
bool TitleUsesBaasNsa(u64 program_id);

// The baasUserID of the linked account, or 0 when it isn't known.
u64 Get();

// The NSA to announce: the baasUserID for the games concerned, the PID otherwise.
u64 Choose(u64 program_id, u64 pid, u64 baas_nsa);

} // namespace WebService::NextendoNsa
