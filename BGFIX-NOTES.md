# BG movie fixes (shadowvfx/itgmania, branch `beta`)

Test build of ITGmania 1.3.0 with these changes:

1. `-random-` BG callouts start at their own beat instead of being moved to
   RandomBGStartBeat (see itgmania/itgmania#1467).
2. Looping movies show their last frame for its full duration and keep
   leftover time when they wrap, so loops don't flash frame 0 or drift.
3. A non-looping movie that reached its end can play again when it is
   reused later in the song.
4. Frame 0 is shown immediately when a movie is rewound.
5. When several frames are due at once (fast rate or music rate), the movie
   skips ahead instead of falling behind.
6. The first update after a movie starts uploads the current frame, so no
   blank or stale frame is drawn.
7. During a BG transition (e.g. CrossFade) the outgoing movie keeps playing
   while it fades instead of freezing.
