// Berserk is a UCI compliant chess engine written in C
// Copyright (C) 2022 Jay Honnold

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef MOVEPICK_H
#define MOVEPICK_H

#include "types.h"

void InitAllMoves(MovePicker* picker, Move hashMove, ThreadData* thread, SearchStack* ss, BitBoard threats);
void InitNoisyMoves(MovePicker* picker, ThreadData* thread, int probcut);
void InitPerftMoves(MovePicker* picker, Board* board);
Move NextMove(MovePicker* picker, Board* board, int skipQuiets);

#endif