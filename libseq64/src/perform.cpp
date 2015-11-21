/*
 *  This file is part of seq24/sequencer64.
 *
 *  seq24 is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  seq24 is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with seq24; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * \file          perform.cpp
 *
 *  This module defines the base class for the performance mode.
 *
 * \library       sequencer64 application
 * \author        Seq24 team; modifications by Chris Ahlstrom
 * \date          2015-07-24
 * \updates       2015-11-21
 * \license       GNU GPLv2 or above
 *
 *  This class is probably the single most important class in Sequencer64, as
 *  it supports sequences, playback, JACK, and more.
 */

#include <sched.h>
#include <stdio.h>

#ifndef PLATFORM_WINDOWS
#include <time.h>                       /* struct timespec                  */
#endif

#include "calculations.hpp"
#include "event.hpp"
#include "keystroke.hpp"
#include "midibus.hpp"
#include "perform.hpp"
#include "sequence.hpp"

namespace seq64
{

static const int c_status_replace  = 0x01;
static const int c_status_snapshot = 0x02;
static const int c_status_queue    = 0x04;

/**
 *  This construction initializes a vast number of member variables, some
 *  of them public (but we're working on that)!
 *
 * \param mygui
 *      Provides access to the GUI assistant that holds many things,
 *      including the containers of keys and the "events" they
 *      provide.  This is a base-class reference; for a real class, see
 *      the gui_assistant_gtk2 class in the seq_gtkmm2 GUI-specific library.
 *      Note that we access the m_gui_support member using the gui()
 *      accessor function.
 */

perform::perform (gui_assistant & mygui, int ppqn)
 :
    m_gui_support               (mygui),
    m_mute_group                (),         // boolean array
    m_tracks_mute_state         (),         // boolean array
    m_mode_group                (true),
    m_mode_group_learn          (false),
    m_mute_group_selected       (0),
    m_playing_screen            (0),
    m_playscreen_offset         (0),
    m_seqs                      (),         // pointer array
    m_seqs_active               (),         // boolean array
    m_was_active_main           (),         // boolean array
    m_was_active_edit           (),         // boolean array
    m_was_active_perf           (),         // boolean array
    m_was_active_names          (),         // boolean array
    m_sequence_state            (),         // boolean array
    m_master_bus                (),
    m_out_thread                (),
    m_in_thread                 (),
    m_out_thread_launched       (false),
    m_in_thread_launched        (false),
    m_running                   (false),
    m_inputing                  (true),
    m_outputing                 (true),
    m_looping                   (false),
    m_playback_mode             (false),
    m_ppqn                      (choose_ppqn(ppqn)),
    m_one_measure               (m_ppqn * 4),
    m_left_tick                 (0),
    m_right_tick                (m_one_measure * 4),        // m_ppqn * 16
    m_starting_tick             (0),
    m_tick                      (0),
    m_usemidiclock              (false),
    m_midiclockrunning          (false),
    m_midiclocktick             (0),
    m_midiclockpos              (-1),
    m_screen_set_notepad        (),     // string array of size c_max_sets
    m_midi_cc_toggle            (),     // midi_control array
    m_midi_cc_on                (),     // midi_control array
    m_midi_cc_off               (),     // midi_control array
    m_offset                    (0),
    m_control_status            (0),
    m_screenset                 (0),
    m_sequence_count            (0),
    m_sequence_max              (c_max_sequence),
    m_is_modified               (false),
    m_condition_var             (),
#ifdef SEQ64_JACK_SUPPORT
    m_jack_asst                 (*this),
#endif
    m_notify                    ()          // vector of pointers, public!
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        m_seqs[i] = nullptr;
        m_seqs_active[i] = false;
    }
    midi_control zero =
    {
        false,          // m_active;
        false,          // m_inverse_active;
        0,              // m_status;
        0,              // m_data;
        0,              // m_min_value;
        0               // m_max_value;
    };
    for (int i = 0; i < c_midi_controls; i++)
    {
        m_midi_cc_toggle[i] = zero;
        m_midi_cc_on[i] = zero;
        m_midi_cc_off[i] = zero;
    }
    set_all_key_events();
    set_all_key_groups();
}

/**
 *  The destructor sets some running flags to false, signals this
 *  condition, then joins the input and output threads if the were
 *  launched. Finally, any active patterns/sequences are deleted.
 */

perform::~perform ()
{
    m_inputing = m_outputing = m_running = false;
    m_condition_var.signal();                   // signal the end
    if (m_out_thread_launched)
        pthread_join(m_out_thread, NULL);

    if (m_in_thread_launched)
        pthread_join(m_in_thread, NULL);

    for (int seq = 0; seq < m_sequence_max; seq++)
    {
        /*
         * This really isn't good enough.  How do we know there aren't some
         * inactive sequences hanging around, due to a call to set_active()?
         * Check all the slots for deletion, and also solidify the deletion by
         * nullifying the pointer.
         *
         * if (is_active(seq))
         *     delete m_seqs[seq];
         */

        if (not_nullptr(m_seqs[seq]))
        {
            delete m_seqs[seq];
            m_seqs[seq] = nullptr;
        }
    }
}

/**
 *  Initializes the master MIDI bus.  Who calls this routine?  The main()
 *  routine of the application.
 */

void
perform::init ()
{
    m_master_bus.init();
}

/**
 *  Initializes JACK support, if SEQ64_JACK_SUPPORT is defined.  Who calls
 *  this routine?  The main() routine of the application, and the options
 *  module.
 */

void
perform::init_jack ()
{
#ifdef SEQ64_JACK_SUPPORT
    m_jack_asst.init();
#endif
}

/**
 *  Tears down the JACK infrastructure.
 */

void
perform::deinit_jack ()
{
#ifdef SEQ64_JACK_SUPPORT
    m_jack_asst.deinit();
#endif
}

/**
 *  Clears all of the patterns/sequences.  The mainwnd module calls this
 *  function.  Note that perform now handles the "is-modified" flag.
 */

void
perform::clear_all ()
{
    reset_sequences();
    for (int i = 0; i < m_sequence_max; i++)
        if (is_active(i))
            delete_sequence(i);

    std::string e;                          /* an empty string  */
    for (int i = 0; i < c_max_sets; i++)
        set_screen_set_notepad(i, e);

    is_modified(false);                     /* new!             */
}

/**
 *  Provides common code to keep the track value valid.  Note the bug we
 *  found, where we checked for track > c_seqs_in_set, but set it to
 *  c_seqs_in_set - 1 in that case!
 */

inline int
perform::clamp_track (int track) const
{
    if (track < 0)
        track = 0;
    else if (track >= c_seqs_in_set)        /* bug: was just ">" !!! */
        track = c_seqs_in_set - 1;

    return track;
}

/**
 * \setter m_mute_group
 */

void
perform::set_group_mute_state (int a_g_track, bool a_mute_state)
{
    int index = clamp_track(a_g_track) + m_mute_group_selected * c_seqs_in_set;
    m_mute_group[index] = a_mute_state;
}

/**
 * \getter m_mute_group
 */

bool
perform::get_group_mute_state (int a_g_track)
{
    int index = clamp_track(a_g_track) + m_mute_group_selected * c_seqs_in_set;
    return m_mute_group[index];
}

/**
 *  Makes some checks and sets the group mute flag.
 */

void
perform::select_group_mute (int a_g_mute)
{
    int gmute = clamp_track(a_g_mute);
    int j = gmute * c_seqs_in_set;
    int k = m_playscreen_offset;
    bool error = false;
    if (m_mode_group_learn)
    {
        for (int i = 0; i < c_seqs_in_set; i++)
        {
            if (is_active(i+k))
            {
                if (not_nullptr_assert(m_seqs[i+k], "select_group_mute"))
                {
                    m_mute_group[i+j] = m_seqs[i+k]->get_playing();
                }
                else
                {
                    error = true;
                    break;
                }
            }
        }
    }
    if (! error)
        m_mute_group_selected = gmute;
}

/**
 *  Sets the group-mute mode, then the group-learn mode,
 *  then notifies all of the notification subscribers.
 */

void
perform::set_mode_group_learn ()
{
    set_mode_group_mute();
    m_mode_group_learn = true;
    for (size_t x = 0; x < m_notify.size(); ++x)
        m_notify[x]->on_grouplearnchange(true);
}

/**
 *  Notifies all of the notification subscribers that group-learn
 *  is being turned off.  Then unsets the group-learn mode flag.
 */

void
perform::unset_mode_group_learn ()
{
    for (size_t x = 0; x < m_notify.size(); ++x)
        m_notify[x]->on_grouplearnchange(false);

    m_mode_group_learn = false;
}

/**
 *  Will need to study this one more closely.
 *
 * \param a_group
 *      Provides the group to mute.  Note that this parameter is
 *      essentially a track or sequence number.
 */

void
perform::select_mute_group (int a_group)
{
    int group = clamp_track(a_group);
    int j = group * c_seqs_in_set;
    int k = m_playscreen_offset;

    /*
     * Should make this assignment contingent upon error.
     */

    m_mute_group_selected = group;
    bool error = false;
    for (int i = 0; i < c_seqs_in_set; i++)
    {
        if ((m_mode_group_learn) && (is_active(i + k)))
        {
            if (not_nullptr_assert(m_seqs[i+k], "select_mute_group"))
                m_mute_group[i+j] = m_seqs[i+k]->get_playing();
            else
            {
                error = true;
                break;
            }
        }
        if (! error)
        {
            int index = i + m_mute_group_selected * c_seqs_in_set;
            m_tracks_mute_state[i] = m_mute_group[index];
        }
    }
}

/**
 *  Will need to study this one more closely.
 */

void
perform::mute_group_tracks ()
{
    if (m_mode_group)
    {
        for (int i = 0; i < c_seqs_in_set; i++)
        {
            for (int j = 0; j < c_seqs_in_set; j++)
            {
                if (is_active(i * c_seqs_in_set + j))
                {
                    if ((i == m_playing_screen) && m_tracks_mute_state[j])
                        sequence_playing_on(i * c_seqs_in_set + j);
                    else
                        sequence_playing_off(i * c_seqs_in_set + j);
                }
            }
        }
    }
}

/**
 *  Select a mute group and then mutes the track in the group.
 */

void
perform::select_and_mute_group (int a_g_group)
{
    select_mute_group(a_g_group);
    mute_group_tracks();
}

/**
 *  Mutes all tracks in the current set of active patterns/sequences.
 */

void
perform::mute_all_tracks ()
{
    for (int i = 0; i < m_sequence_max; i++)    // TRY sequence_count()
    {
        if (is_active(i))
            m_seqs[i]->set_song_mute(true);
    }
}

/**
 *  Set the left marker at the given tick.
 *
 * \param tick
 *      The tick (MIDI pulse) at which to place the left tick.  If the left
 *      tick is greater than or equal to the right tick, then the right ticked
 *      is moved forward by one "measure's length" (m_ppqn * 4) past the left
 *      tick.
 *
 * \param setstart
 *      If true (the default, and longstanding implicit setting), then the
 *      starting tick is also set to the left tick.
 */

void
perform::set_left_tick (long tick, bool setstart)
{
    m_left_tick = tick;
    if (setstart)
        set_start_tick(tick);

    if (m_left_tick >= m_right_tick)
        m_right_tick = m_left_tick + m_one_measure;
}

/**
 *  Set the right marker at the given tick.  This setting is made only if the
 *  tick parameter is at or beyond the first measure.
 *
 * \param tick
 *      The tick (MIDI pulse) at which to place the right tick.  If less than
 *      or equal to the left tick setting, then the left tick is backed up by
 *      one "measure's worth" (m_ppqn * 4) worth of ticks from the new right
 *      tick.
 *
 * \param setstart
 *      If true (the default, and longstanding implicit setting), then the
 *      starting tick is also set to the left tick, if that got changed.
 */

void
perform::set_right_tick (long tick, bool setstart)
{
    if (tick >= m_one_measure)
    {
        m_right_tick = tick;
        if (m_right_tick <= m_left_tick)
        {
            m_left_tick = m_right_tick - m_one_measure;
            if (setstart)
                set_start_tick(m_left_tick);
        }
    }
}

/**
 *  True if a sequence is empty and should be highlighted.
 */

#if SEQ64_HIGHLIGHT_EMPTY_SEQS

bool
perform::highlight (const sequence & seq) const
{
    return seq.event_count() == 0;
}

#else

bool
perform::highlight (const sequence & /*seq*/) const
{
    return false;
}

#endif

/**
 *  A private helper function for add_sequence().  It is common code
 *  and using it prevents inconsistences.  It assumes values have already been
 *  checked.
 *
 * \param seq
 *      The pointer to the pattern/sequence to add.
 *
 * \param seqnum
 *      The sequence number of the pattern to be added.
 *
 */

void
perform::install_sequence (sequence * seq, int seqnum)
{
    if (not_nullptr(m_seqs[seqnum]))
    {
        errprintf("install_sequence(): m_seqs[%d] not null\n", seqnum);
        delete m_seqs[seqnum];
        m_seqs[seqnum] = nullptr;
        --m_sequence_count;
    }
    m_seqs[seqnum] = seq;
    if (not_nullptr(seq))
    {
        set_active(seqnum, true);
        ++m_sequence_count;
    }
}

/**
 *  Adds a pattern/sequence pointer to the list of patterns.  No check is made
 *  for a null pointer.
 *
 *  This function checks for the preferred sequence number.  This is the
 *  number that was specified by the Sequence Number meta-event for the
 *  current track.  If the preferred sequence number is in the valid range (0
 *  to m_sequence_max) and it is not active, add it and activate it.
 *
 *  Otherwise, iterate through all patterns from prefnum to m_sequence_max and
 *  add and activate the first one that is not active, and then quit.
 *
 * \warning
 *      The logic of the if-statement in this function was such that \a
 *      prefnum could be out-of-bounds in the else-clause.  We reworked
 *      the logic to be airtight.  This bug was caught by gcc 4.8.3 on CentOS,
 *      but not on gcc 4.9.3 on Debian Sid!
 *
 * \param seq
 *      The pointer to the pattern/sequence to add.
 *
 * \param prefnum
 *      The preferred sequence number of the pattern, as explained above.  If
 *      this value is out-of-range, then it is basically ignored.
 */

void
perform::add_sequence (sequence * seq, int prefnum)
{
    if (! is_seq_valid(prefnum))
        prefnum = 0;

    if (is_active(prefnum))                 /* look for next inactive one   */
    {
        for (int i = prefnum; i < m_sequence_max; i++)
        {
            if (! is_active(i))
            {
                install_sequence(seq, i);
                break;
            }
        }
    }
    else
        install_sequence(seq, prefnum);
}

/**
 *  Sets or unsets the active state of the given pattern/sequence number.
 *  If setting it active, the sequence::number() setter is called. It won't
 *  modify the sequence's internal copy of the sequence number if it has
 *  already been set.
 *
 * \param seq
 *      Provides the prospective sequence number.
 *
 * \param active
 *      True if the sequence is to be set to the active state.
 */

void
perform::set_active (int seq, bool active)
{
    if (is_seq_valid(seq))
    {
        if (m_seqs_active[seq] && ! active)
            set_was_active(seq);

        m_seqs_active[seq] = active;
        if (active && not_nullptr(m_seqs[seq]))
        {
            m_seqs[seq]->number(seq);
            if (m_seqs[seq]->name().empty())
                m_seqs[seq]->set_name(std::string("Untitled"));
        }
    }
}

/**
 *  Sets was-active flags:  main, edit, perf, and names.
 *
 *  Why do we need this routine?
 *
 * \param seq
 *      The pattern number.  It is checked for invalidity.
 */

void
perform::set_was_active (int seq)
{
    if (is_seq_valid(seq))
    {
        m_was_active_main[seq] = true;
        m_was_active_edit[seq] = true;
        m_was_active_perf[seq] = true;
        m_was_active_names[seq] = true;
    }
}

/**
 *  Checks the pattern/sequence for main-dirtiness.  See the
 *  sequence::is_dirty_main() function.
 *
 * \param seq
 *      The pattern number.  It is checked for invalidity.
 *
 * \return
 *      Returns the was-active-main flag value, before setting it to
 *      false.  Returns false if the pattern was invalid.
 */

bool
perform::is_dirty_main (int seq)
{
    bool was_active = false;
    if (sequence_count() > 0)
    {
        if (is_active(seq))
        {
            if (not_nullptr(m_seqs[seq]))
                was_active = m_seqs[seq]->is_dirty_main();
        }
        else
        {
            was_active = m_was_active_main[seq];
            m_was_active_main[seq] = false;
        }
    }
    return was_active;
}

/**
 *  Checks the pattern/sequence for edit-dirtiness.
 *
 * \param seq
 *      The pattern number.  It is checked for invalidity.
 *
 * \return
 *      Returns the was-active-edit flag value, before setting it to
 *      false. Returns false if the pattern was invalid.
 */

bool
perform::is_dirty_edit (int seq)
{
    bool was_active = false;
    if (sequence_count() > 0)
    {
        if (is_active(seq))
        {
            if (not_nullptr(m_seqs[seq]))
                was_active = m_seqs[seq]->is_dirty_edit();
        }
        else
        {
            was_active = m_was_active_edit[seq];
            m_was_active_edit[seq] = false;
        }
    }
    return was_active;
}

/**
 *  Checks the pattern/sequence for perf-dirtiness.
 *
 * \param seq
 *      The pattern number.  It is checked for invalidity.
 *
 * \return
 *      Returns the was-active-perf flag value, before setting it to
 *      false.  Returns false if the pattern/sequence number was invalid.
 */

bool
perform::is_dirty_perf (int seq)
{
    bool was_active = false;
    if (sequence_count() > 0)
    {
        if (is_active(seq))
        {
            if (not_nullptr(m_seqs[seq]))
                was_active = m_seqs[seq]->is_dirty_perf();
        }
        else
        {
            was_active = m_was_active_perf[seq];
            m_was_active_perf[seq] = false;
        }
    }
    return was_active;
}

/**
 *  Checks the pattern/sequence for names-dirtiness.
 *
 * \param seq
 *      The pattern number.  It is checked for invalidity.
 *
 * \return
 *      Returns the was-active-names flag value, before setting it to
 *      false.  Returns false if the pattern/sequence number was invalid.
 */

bool
perform::is_dirty_names (int seq)
{
    bool was_active = false;
    if (sequence_count() > 0)
    {
        if (is_active(seq))
        {
            if (not_nullptr(m_seqs[seq]))
                was_active = m_seqs[seq]->is_dirty_names();
        }
        else
        {
            was_active = m_was_active_names[seq];
            m_was_active_names[seq] = false;
        }
    }
    return was_active;
}

/**
 *  Sets the value of the BPM into the master MIDI buss, after making
 *  sure it is squelched to be between 20 and 500.
 *
 *  The value is set only if neither JACK nor this performance object are
 *  running.
 *
 *  It's not clear that we need to set the is-modified flag just because we
 *  changed the screen set.
 */

void
perform::set_beats_per_minute (int bpm)
{
    if (bpm < SEQ64_MINIMUM_BPM)        /*  20 */
        bpm = SEQ64_MINIMUM_BPM;

    if (bpm > SEQ64_MAXIMUM_BPM)        /* 500 */
        bpm = SEQ64_MAXIMUM_BPM;

    /**
     * \todo
     *      I think this logic is wrong, in that it needs only one of the
     *      two to be stopped before it sets the BPM, while it seems to me
     *      that both should be stopped; to be determined.
     */

    if (! (m_jack_asst.is_running() && m_running))
        m_master_bus.set_beats_per_minute(bpm);

    /*
     * modify(true);                // iffy!
     */
}

/**
 *  Retrieves the BPM setting of the master MIDI buss.
 */

int
perform::get_beats_per_minute ()
{
    return m_master_bus.get_beats_per_minute();
}

/**
 *  Provides common code to check for the bounds of a sequence number.
 *  Also see the function is_mseq_valid(), which also checks the pointer
 *  stored in the m_seq[] array.
 *
 *  We considered checking the \a seq param against sequence_count(), but
 *  this function is called while creating sequences that add to that count,
 *  so we continue checking against the "container" size.  Also, it is
 *  possible to have holes in the array representing inactive sequences,
 *  so that sequencer_count() would be too limiting.
 *
 * \param seq
 *      The sequencer number, in interval [0, m_sequence_max).
 *
 * \return
 *      Returns true if the sequence number is valid.
 */

bool
perform::is_seq_valid (int seq) const
{
    if (seq >= 0 && seq < m_sequence_max)
        return true;
    else
    {
        if (! SEQ64_IS_DISABLED_SEQUENCE(seq))  /* legal value, don't bitch */
        {
            fprintf
            (
                stderr, "is_seq_valid(): seq = %d > %d\n",
                seq, m_sequence_max-1
            );
        }
        return false;
    }
}

/**
 *  Validates the sequence number, which is important since they're currently
 *  used as array indices.  It also evaluates the m_seq[seq] pointer value.
 *
 * \note
 *      Since we can have holes in the sequence array, where there are
 *      inactive sequences, we check if the sequence is even active before
 *      emitting a message about a null pointer for the sequence.  We only
 *      want to see messages that indicate actual problems.
 *
 * \param seq
 *      Provides the sequence number to be checked.  It is checked for
 *      validity.  We cannot compare the sequence number versus the
 *      sequence_count(), because the current implementation can have inactive
 *      holes (with null pointers) interspersed with active pointers.
 *
 * \return
 *      Returns true if the sequence number is valid as per is_seq_valid(),
 *      and the sequence pointer is not null.
 */

bool
perform::is_mseq_valid (int seq) const
{
    bool result = is_seq_valid(seq);
    if (result)
    {
        result = not_nullptr(m_seqs[seq]);
        if (! result && m_seqs_active[seq])
        {
            errprintf("is_mseq_valid(): m_seqs[%d] is null\n", seq);
        }
    }
    return result;
}

/**
 *  Deletes a pattern/sequence by number.  We now also solidify the deletion
 *  by setting the pointer to null after deletion.
 */

void
perform::delete_sequence (int seq)
{
    if (is_mseq_valid(seq))
    {
        set_active(seq, false);
        if (! m_seqs[seq]->get_editing())
        {
            m_seqs[seq]->set_playing(false);
            delete m_seqs[seq];
            m_seqs[seq] = nullptr;
        }
    }
}

/**
 *  Check if the pattern/sequence, given by number, has an edit in
 *  progress.
 */

bool
perform::is_sequence_in_edit (int seq)
{
    if (is_mseq_valid(seq))
        return m_seqs[seq]->get_editing();
    else
        return false;
}

/**
 *  Creates a new pattern/sequence for the given slot, and sets the new
 *  pattern's master MIDI bus address.  Then it activates the pattern.
 *
 *  It doesn't deal with thrown exceptions.
 */

void
perform::new_sequence (int seq)
{
    if (is_seq_valid(seq))
    {
        m_seqs[seq] = new sequence();
        m_seqs[seq]->set_master_midi_bus(&m_master_bus);
        set_active(seq, true);
    }
}

/**
 *  Retrieves a value from m_midi_cc_toggle[].
 *
 * \param seq
 *      Provides a control value (such as c_midi_control_bpm_up) to use to
 *      retrieve the desired midi_control object.  Note that this value is
 *      unsigned simply to make the legality check of the parameter
 *      easier.
 */

midi_control *
perform::get_midi_control_toggle (unsigned int seq)
{
    return is_midi_control_valid(seq) ? &m_midi_cc_toggle[seq] : nullptr ;
}

/**
 *  Retrieves a value from m_midi_cc_on[].
 *
 * \param seq
 *      Provides a control value (such as c_midi_control_bpm_up) to use to
 *      retrieve the desired midi_control object.
 */

midi_control *
perform::get_midi_control_on (unsigned int seq)
{
    return is_midi_control_valid(seq) ? &m_midi_cc_on[seq] : nullptr ;
}

/**
 *  Retrieves a value from m_midi_cc_off[].
 *
 * \param seq
 *      Provides a control value (such as c_midi_control_bpm_up) to use to
 *      retrieve the desired midi_control object.
 */

midi_control *
perform::get_midi_control_off (unsigned int seq)
{
    return is_midi_control_valid(seq) ? &m_midi_cc_off[seq] : nullptr ;
}

/**
 *  Copies the given string into m_screen_set_notepad[].
 *
 * \param screenset
 *      The ID number of the string set, an index into the
 *      m_screen_set_xxx[] arrays.
 *
 * \param notepad
 *      Provides the string date to copy into the notepad.
 *      Not sure why a pointer is used, instead of nice "const std::string
 *      &" parameter.  And this pointer isn't checked.
 */

void
perform::set_screen_set_notepad (int screenset, const std::string & notepad)
{
    if (is_screenset_valid(screenset))
    {
        if (notepad != m_screen_set_notepad[screenset])
        {
            m_screen_set_notepad[screenset] = notepad;
            modify();
        }
    }
}

/**
 *  Retrieves the given string from m_screen_set_notepad[].
 *
 * \param screenset
 *      The ID number of the string set, an index into the
 *      m_screen_set_notepad[] array.  This value is validated.
 *
 * \return
 *      Returns a reference to the desired string, or to an empty string
 *      if the screen-set number is invalid.
 */

const std::string &
perform::get_screen_set_notepad (int screenset) const
{
    static std::string s_empty;
    if (is_screenset_valid(screenset))
        return m_screen_set_notepad[screenset];
    else
        return s_empty;
}

/**
 *  Sets the m_screenset value (the index or ID of the current screen
 *  set).
 *
 *  It's not clear that we need to set the is-modified flag just because we
 *  changed the screen set.
 *
 * \param ss
 *      The index of the desired string set.  It is forced to range from
 *      0 to c_max_sets - 1.
 */

void
perform::set_screenset (int ss)
{
    if (ss < 0)
        ss = c_max_sets - 1;
    else if (ss >= c_max_sets)
        ss = 0;

    if (ss != m_screenset)
    {
        m_screenset = ss;

        /*
         * modify(true);                // iffy, so we comment it out
         */
    }
}

/**
 *  Sets the screen set that is active, based on the value of
 *  m_playing_screen.
 *
 *  For each value up to c_seqs_in_set (32), the index of the current
 *  sequence in the currently screen set (m_playing_screen) is obtained.
 *  If it is active and the sequence actually exists
 *
 *  Modifies m_playing_screen, and mutes the group tracks.
 */

void
perform::set_playing_screenset ()
{
    /*
     * This loop could be changed to avoid the multiplication.
     */

    bool error = false;
    for (int j, i = 0; i < c_seqs_in_set; i++)
    {
        j = i + m_playscreen_offset;    /* m_playing_screen * c_seqs_in_set */
        if (is_active(j))
        {
            /*
             * Not a big fan of asserts in production code, since they do
             * nothing; we still want to have a way to report null
             * pointers in production code, and keep going.  So we wrote
             * a function in the easy_macros module to do it.  Please note
             * that enabling debug mode with these asserts really slows
             * down the loading of a MIDI file, well, at least when using
             * std::list and sorting.
             */

            if (not_nullptr_assert(m_seqs[j], "set_playing_screenset"))
                m_tracks_mute_state[i] = m_seqs[j]->get_playing();
            else
            {
                error = true;
                break;
            }
        }
    }

    /*
     * Do we need to avoid doing this if the pointer is null? No.
     */

    if (! error)
    {
        m_playing_screen = m_screenset;
        m_playscreen_offset = m_playing_screen * c_seqs_in_set;
        mute_group_tracks();
    }
}

/**
 *  Starts the playing of all the patterns/sequences.
 *
 *  This function just runs down the list of sequences and has them dump
 *  their events.
 *
 * \param tick
 *      Provides the tick at which to start playing.
 */

void
perform::play (long tick)
{
    m_tick = tick;
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "play"))
            {
                /*
                 * Check the number of events.  This doesn't stop the progress
                 * bar update for an empty sequence, though.
                 */

                if (m_seqs[i]->event_count() == 0)      /* new 2015-08-23 */
                    continue;

                if
                (
                    m_seqs[i]->get_queued() &&
                    m_seqs[i]->get_queued_tick() <= tick
                )
                {
                    m_seqs[i]->play
                    (
                        m_seqs[i]->get_queued_tick() - 1, m_playback_mode
                    );
                    m_seqs[i]->toggle_playing();
                }
                m_seqs[i]->play(tick, m_playback_mode);
            }
        }
    }
    m_master_bus.flush();               /* flush the MIDI buss  */
}

/**
 *  For every pattern/sequence that is active, sets the "original ticks"
 *  value for the pattern.
 *
 * \param tick
 */

void
perform::set_orig_ticks (long tick)
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "set_orig_ticks"))
                m_seqs[i]->set_orig_tick(tick);
        }
    }
}

/**
 *  Clears the patterns/sequence for the given sequence, if it is active.
 *
 * \param seq
 *      Provides the desired sequence.  Hopefull, the is_active() function
 *      validates this value.
 */

void
perform::clear_sequence_triggers (int seq)
{
    if (is_active(seq))
    {
        if (not_nullptr_assert(m_seqs[seq], "clear_sequence_triggers"))
            m_seqs[seq]->clear_triggers();
    }
}

/**
 *  If the left tick is less than the right tick, then, for each sequence
 *  that is active, its triggers are moved by the difference between the
 *  right and left in the specified direction.
 *
 * \param a_direction
 *      Specifies the desired direction; false = left, true = right.
 */

void
perform::move_triggers (bool a_direction)
{
    if (m_left_tick < m_right_tick)
    {
        long distance = m_right_tick - m_left_tick;
        for (int i = 0; i < m_sequence_max; i++)
        {
            if (is_active(i))
            {
                if (not_nullptr_assert(m_seqs[i], "move_triggers"))
                    m_seqs[i]->move_triggers(m_left_tick, distance, a_direction);
            }
        }
    }
}

/**
 *  For every active sequence, call that sequence's push_trigger_undo()
 *  function.  Too bad we cannot yet keep track of all the undoes.
 */

void
perform::push_trigger_undo ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "push_trigger_undo"))
                m_seqs[i]->push_trigger_undo();
        }
    }
}

/**
 *  For every active sequence, call that sequence's pop_trigger_undo()
 *  function.
 */

void
perform::pop_trigger_undo ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "pop_trigger_undo"))
                m_seqs[i]->pop_trigger_undo();
        }
    }
}

/**
 *  If the left tick is less than the right tick, then, for each sequence
 *  that is active, its triggers are copied, offset by the difference
 *  between the right and left.
 *
 *  This copies the triggers between the L marker and R marker to the R
 *  marker.
 */

void
perform::copy_triggers ()
{
    if (m_left_tick < m_right_tick)
    {
        long distance = m_right_tick - m_left_tick;
        for (int i = 0; i < m_sequence_max; i++)
        {
            if (is_active(i))
            {
                if (not_nullptr_assert(m_seqs[i], "copy_triggers"))
                    m_seqs[i]->copy_triggers(m_left_tick, distance);
            }
        }
    }
}

/**
 *  If JACK is supported, starts the JACK transport.
 */

void
perform::start_jack ()
{
#ifdef SEQ64_JACK_SUPPORT
    m_jack_asst.start();
#endif
}

/**
 *  If JACK is supported, stops the JACK transport.
 */

void
perform::stop_jack ()
{
#ifdef SEQ64_JACK_SUPPORT
    m_jack_asst.stop();
#endif
}

/**
 *  If JACK is supported and running, sets the position of the transport.
 */

void
perform::position_jack (bool state)
{
#ifdef SEQ64_JACK_SUPPORT
    m_jack_asst.position(state);
#endif
}

/**
 *  If JACK is not running, call inner_start() with the given state.
 *
 * \param state
 *      What does this state mean?
 */

void
perform::start (bool state)
{
    if (! m_jack_asst.is_running())
        inner_start(state);
}

/**
 *  If JACK is not running, call inner_stop().
 *
 *  The logic seems backward here, in that we call inner_stop() if JACK is
 *  not running.  Or perhaps we misunderstand the meaning of
 *  m_jack_running?
 */

void
perform::stop ()
{
    if (! m_jack_asst.is_running())
        inner_stop();
}

/**
 *  Locks on m_condition_var.  Then, if not is_running(), the playback
 *  mode is set to the given state.  If that state is true, call
 *  off_sequences().  Set the running status, and signal the condition.
 *  Then unlock.
 */

void
perform::inner_start (bool state)
{
    m_condition_var.lock();
    if (! is_running())
    {
        set_playback_mode(state);
        if (state)
            off_sequences();

        set_running(true);
        m_condition_var.signal();
    }
    m_condition_var.unlock();
}

/**
 *  Unconditionally, and without locking, clears the running status,
 *  resets the sequences, and set m_usemidiclock false.
 */

void
perform::inner_stop ()
{
    set_running(false);
    reset_sequences();                 // off_sequences();
    m_usemidiclock = false;
}

/**
 *  For all active patterns/sequences, set the playing state to false.
 */

void
perform::off_sequences ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "off_sequences"))
                m_seqs[i]->set_playing(false);
        }
    }
}

/**
 *  For all active patterns/sequences, turn off its playing notes.
 *  Then flush the MIDI buss.
 */

void
perform::all_notes_off ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "all_notes_off"))
                m_seqs[i]->off_playing_notes();
        }
    }
    m_master_bus.flush();              // flush the MIDI buss
}

/**
 *  For all active patterns/sequences, get its playing state, turn off the
 *  playing notes, set playing to false, zero the markers, and, if not in
 *  playback mode, restore the playing state.
 *
 *  Then flush the MIDI buss.
 */

void
perform::reset_sequences ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "reset_sequences"))
            {
                bool state = m_seqs[i]->get_playing();
                m_seqs[i]->off_playing_notes();
                m_seqs[i]->set_playing(false);
                m_seqs[i]->zero_markers();
                if (! m_playback_mode)
                    m_seqs[i]->set_playing(state);
            }
        }
    }
    m_master_bus.flush();              // flush the MIDI bus
}

/**
 *  Creates the output thread using output_thread_func().
 *
 *  This might be a good candidate for a small thread class derived from a
 *  small base class.
 */

void
perform::launch_output_thread ()
{
    int err = pthread_create(&m_out_thread, NULL, output_thread_func, this);
    if (err != 0)
    {
        /* TODO: error handling */
    }
    else
        m_out_thread_launched = true;
}

/**
 *  Creates the input thread using input_thread_func().
 *
 *  This might be a good candidate for a small thread class derived from a
 *  small base class.
 */

void
perform::launch_input_thread ()
{
    int err = pthread_create(&m_in_thread, NULL, input_thread_func, this);
    if (err != 0)
    {
        /* TODO: error handling */
    }
    else
        m_in_thread_launched = true;
}

/**
 *  Locates the largest trigger value among the active sequences.
 *
 * \return
 *      Returns the highest trigger value, or zero.  It is not clear why
 *      this function doesn't return a "no trigger found" value. Is there
 *      always at least one trigger, at 0?
 */

long
perform::get_max_trigger ()
{
    long result = 0;
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "get_max_trigger"))
            {
                long t = m_seqs[i]->get_max_trigger();
                if (t > result)
                    result = t;
            }
        }
    }
    return result;
}

/**
 *  Set up the performance, and set the process to realtime privileges.
 */

void *
output_thread_func (void * a_pef)
{
    perform * p = (perform *) a_pef;
    if (not_nullptr_assert(p, "output_thread_func"))
    {
        if (rc().priority())
        {
            struct sched_param schp;
            memset(&schp, 0, sizeof(sched_param));
            schp.sched_priority = 1;

#ifndef PLATFORM_WINDOWS            // Not in MinGW RCB
            if (sched_setscheduler(0, SCHED_FIFO, &schp) != 0)
            {
                errprint
                (
                    "output_thread_func: couldn't sched_setscheduler"
                    " (FIFO), need to be root."
                );
                pthread_exit(0);
            }
#endif
        }

#ifdef PLATFORM_WINDOWS
        timeBeginPeriod(1);
        p->output_func();
        timeEndPeriod(1);
#else
        p->output_func();
#endif
    }
    return 0;
}

/**
 *  Performance output function.
 */

void
perform::output_func ()
{
    while (m_outputing)
    {
        m_condition_var.lock();        // printf ("waiting for signal\n");
        while (! m_running)
        {
            m_condition_var.wait();
            if (! m_outputing)          // if stopping, then kill thread
                break;
        }
        m_condition_var.unlock();

        jack_scratchpad pad;
        pad.js_current_tick = 0.0;      // tick and tick fraction
        pad.js_total_tick = 0.0;
        pad.js_clock_tick = 0.0;
        pad.js_jack_stopped = false;
        pad.js_dumping = false;
        pad.js_init_clock = true;
        pad.js_looping = m_looping;
        pad.js_playback_mode = m_playback_mode;

#ifndef PLATFORM_WINDOWS
        struct timespec last;               // beginning time
        struct timespec current;            // current time
        struct timespec stats_loop_start;
        struct timespec stats_loop_finish;
        struct timespec delta;              // difference between last & current
#else
        long last;                          // beginning time
        long current;                       // current time
        long stats_loop_start = 0;
        long stats_loop_finish = 0;
        long delta;                         // difference between last & current
#endif

        long stats_total_tick = 0;
        long stats_loop_index = 0;
        long stats_min = 0x7FFFFFFF;
        long stats_max = 0;
        long stats_avg = 0;
        long stats_last_clock_us = 0;
        long stats_clock_width_us = 0;
        long stats_all[100];
        long stats_clock[100];
        for (int i = 0; i < 100; i++)
        {
            stats_all[i] = 0;
            stats_clock[i] = 0;
        }

        /*
         * If we are in the performance view (song editor), we care about
         * starting from the offset.
         */

        if (m_playback_mode && ! m_jack_asst.is_running())
        {
            pad.js_current_tick = m_starting_tick;
            pad.js_clock_tick = m_starting_tick;
            set_orig_ticks(m_starting_tick);
        }

        int ppqn = m_master_bus.get_ppqn();
#ifndef PLATFORM_WINDOWS
        clock_gettime(CLOCK_REALTIME, &last);   // get start time position
        if (rc().stats())
            stats_last_clock_us = (last.tv_sec*1000000) + (last.tv_nsec/1000);
#else
        last = timeGetTime();                   // get start time position
        if (rc().stats())
            stats_last_clock_us = last * 1000;
#endif

        while (m_running)
        {
            /**
             * -# Get delta time (current - last).
             * -# Get delta ticks from time.
             * -# Add to current_ticks.
             * -# Compute prebuffer ticks.
             * -# Play from current tick to prebuffer.
             */

            if (rc().stats())
            {
#ifndef PLATFORM_WINDOWS
                clock_gettime(CLOCK_REALTIME, &stats_loop_start);
#else
                stats_loop_start = timeGetTime();
#endif
            }

            /*
             * Get the delta time.
             */

#ifndef PLATFORM_WINDOWS
            clock_gettime(CLOCK_REALTIME, &current);
            delta.tv_sec  = (current.tv_sec  - last.tv_sec);
            delta.tv_nsec = (current.tv_nsec - last.tv_nsec);
            long delta_us = (delta.tv_sec * 1000000) + (delta.tv_nsec / 1000);
#else
            current = timeGetTime();
            delta = current - last;
            long delta_us = delta * 1000;
#endif
            int bpm  = m_master_bus.get_beats_per_minute();

            /*
             * Delta time to ticks; get delta ticks.
             *
             * double delta_tick = double(bpm * ppqn * (delta_us / 60000000.0f));
             */

            double delta_tick = delta_time_us_to_ticks(bpm, ppqn, delta_us);
            if (m_usemidiclock)
            {
                delta_tick = m_midiclocktick;
                m_midiclocktick = 0;
            }
            if (0 <= m_midiclockpos)
            {
                delta_tick = 0;
                pad.js_clock_tick = pad.js_current_tick =
                    pad.js_total_tick = m_midiclockpos;

                m_midiclockpos = -1;

                // init_clock = true;
            }

#ifdef SEQ64_JACK_SUPPORT
            bool jackrunning = m_jack_asst.output(pad);
            if (jackrunning)
            {
                // code?
            }
            else
#endif
            {
                /*
                 * The default if JACK is not compiled in, or is not running.
                 * Add the delta to the current ticks.
                 */

                pad.js_clock_tick += delta_tick;
                pad.js_current_tick += delta_tick;
                pad.js_total_tick += delta_tick;
                pad.js_dumping = true;
            }

            /*
             * init_clock will be true when we run for the first time, or
             * as soon as JACK gets a good lock on playback.
             */

            if (pad.js_init_clock)
            {
                m_master_bus.init_clock(long(pad.js_clock_tick));
                pad.js_init_clock = false;
            }
            if (pad.js_dumping)
            {
                if (m_looping && m_playback_mode)
                {
                    if (pad.js_current_tick >= get_right_tick())
                    {
                        double leftover_tick =
                            pad.js_current_tick-(get_right_tick());

                        play(get_right_tick() - 1);             // play!
                        reset_sequences();                      // reset!
                        set_orig_ticks(get_left_tick());
                        pad.js_current_tick =
                            double(get_left_tick() + leftover_tick);
                    }
                }
                play(long(pad.js_current_tick));                // play!
                m_master_bus.clock(long(pad.js_clock_tick));    // MIDI clock
                if (rc().stats())
                {
                    while (stats_total_tick <= pad.js_total_tick)
                    {
                        int ct = clock_ticks_from_ppqn(m_ppqn);
                        if ((stats_total_tick % ct) == 0)   /* there a tick ? */
                        {

#ifndef PLATFORM_WINDOWS
                            long current_us =
                                (current.tv_sec * 1000000) +
                                (current.tv_nsec / 1000);
#else
                            long current_us = current * 1000;
#endif
                            stats_clock_width_us = current_us-stats_last_clock_us;
                            stats_last_clock_us = current_us;

                            int index = stats_clock_width_us / 300;
                            if (index >= 100)
                                index = 99;

                            stats_clock[index]++;
                        }
                        stats_total_tick++;
                    }
                }
            }

            /**
             *  Figure out how much time we need to sleep, and do it.
             */

            last = current;

#ifndef PLATFORM_WINDOWS
            clock_gettime(CLOCK_REALTIME, &current);
            delta.tv_sec  = (current.tv_sec  - last.tv_sec);
            delta.tv_nsec = (current.tv_nsec - last.tv_nsec);
            long elapsed_us = (delta.tv_sec * 1000000) + (delta.tv_nsec / 1000);
#else
            current = timeGetTime();
            delta = current - last;
            long elapsed_us = delta * 1000;
#endif

            /*
             * Now we want to trigger every c_thread_trigger_width_ms,
             * and it took us delta_us to play().
             */

            delta_us = (c_thread_trigger_width_ms * 1000) - elapsed_us;

            /*
             * Check MIDI clock adjustment.  Note that we replaced
             * "60000000.0f / m_ppqn / bpm" with a call to a function.
             * We also removed the "f" specification from the constants.
             */

            double dct = double_ticks_from_ppqn(m_ppqn);
            double next_total_tick = pad.js_total_tick + dct;
            double next_clock_delta = next_total_tick - pad.js_total_tick - 1;
            double next_clock_delta_us =
                next_clock_delta * pulse_length_us(bpm, m_ppqn);

            if (next_clock_delta_us < (c_thread_trigger_width_ms * 1000.0 * 2.0))
                delta_us = long(next_clock_delta_us);

#ifndef PLATFORM_WINDOWS                    // nanosleep() is actually Linux
            if (delta_us > 0)
            {
                delta.tv_sec = (delta_us / 1000000);
                delta.tv_nsec = (delta_us % 1000000) * 1000;
                nanosleep(&delta, NULL);
            }
#else
            if (delta_us > 0)
            {
                delta = (delta_us / 1000);
                Sleep(delta);
            }
#endif
            else
            {
                if (rc().stats())
                    printf("underrun\n");
            }

            if (rc().stats())
            {
#ifndef PLATFORM_WINDOWS
                clock_gettime(CLOCK_REALTIME, &stats_loop_finish);
#else
                stats_loop_finish = timeGetTime();
#endif
            }

            if (rc().stats())
            {

#ifndef PLATFORM_WINDOWS
                delta.tv_sec  = stats_loop_finish.tv_sec-stats_loop_start.tv_sec;
                delta.tv_nsec = stats_loop_finish.tv_nsec-stats_loop_start.tv_nsec;
                long delta_us = (delta.tv_sec*1000000) + (delta.tv_nsec/1000);
#else
                delta = stats_loop_finish - stats_loop_start;
                long delta_us = delta * 1000;
#endif
                int index = delta_us / 100;
                if (index >= 100)
                    index = 99;

                stats_all[index]++;
                if (delta_us > stats_max)
                    stats_max = delta_us;

                if (delta_us < stats_min)
                    stats_min = delta_us;

                stats_avg += delta_us;
                stats_loop_index++;
                if (stats_loop_index > 200)
                {
                    stats_loop_index = 0;
                    stats_avg /= 200;
                    printf
                    (
                        "stats_avg[%ld]us stats_min[%ld]us stats_max[%ld]us\n",
                        stats_avg, stats_min, stats_max
                    );
                    stats_min = 0x7FFFFFFF;
                    stats_max = 0;
                    stats_avg = 0;
                }
            }
            if (pad.js_jack_stopped)
                inner_stop();
        }

        if (rc().stats())
        {
            printf("\n\n-- trigger width --\n");
            for (int i = 0; i < 100; i++)
            {
                printf("[%3d][%8ld]\n", i * 100, stats_all[i]);
            }
            printf("\n\n-- clock width --\n");
            int bpm  = m_master_bus.get_beats_per_minute();
            printf
            (
                "optimal: [%d us]\n", int(clock_tick_duration_bogus(bpm, m_ppqn))
            );
            for (int i = 0; i < 100; i++)
            {
                printf("[%3d][%8ld]\n", i * 300, stats_clock[i]);
            }
        }
        m_tick = 0;
        m_master_bus.flush();
        m_master_bus.stop();
    }
    pthread_exit(0);
}

/**
 *  Set up the performance, and set the process to realtime privileges.
 */

void *
input_thread_func (void * a_pef)
{
    perform * p = (perform *) a_pef;
    if (not_nullptr_assert(p, "input_thread_func"))
    {
        if (rc().priority())
        {
            struct sched_param schp;
            memset(&schp, 0, sizeof(sched_param));
            schp.sched_priority = 1;

#ifndef PLATFORM_WINDOWS                // MinGW RCB
            if (sched_setscheduler(0, SCHED_FIFO, &schp) != 0)
            {
                printf("input_thread_func: couldnt sched_setscheduler"
                       " (FIFO), you need to be root.\n");
                pthread_exit(0);
            }
#endif
        }
#ifdef PLATFORM_WINDOWS
        timeBeginPeriod(1);
        p->input_func();
        timeEndPeriod(1);
#else
        p->input_func();
#endif
    }
    return 0;
}

/**
 *  Handle the MIDI Control values that provide some automation for the
 *  application.
 */

void
perform::handle_midi_control (int a_control, bool state)
{
    switch (a_control)
    {
    case c_midi_control_bpm_up:                 // printf("bpm up\n");
        set_beats_per_minute(get_beats_per_minute() + 1);
        break;

    case c_midi_control_bpm_dn:                 // printf("bpm dn\n");
        set_beats_per_minute(get_beats_per_minute() - 1);
        break;

    case c_midi_control_ss_up:                  // printf("ss up\n");
        set_screenset(get_screenset() + 1);
        break;

    case c_midi_control_ss_dn:                  // printf("ss dn\n");
        set_screenset(get_screenset() - 1);
        break;

    case c_midi_control_mod_replace:            // printf("replace\n");
        if (state)
            set_sequence_control_status(c_status_replace);
        else
            unset_sequence_control_status(c_status_replace);
        break;

    case c_midi_control_mod_snapshot:           // printf("snapshot\n");
        if (state)
            set_sequence_control_status(c_status_snapshot);
        else
            unset_sequence_control_status(c_status_snapshot);
        break;

    case c_midi_control_mod_queue:              // printf("queue\n");
        if (state)
            set_sequence_control_status(c_status_queue);
        else
            unset_sequence_control_status(c_status_queue);

    case c_midi_control_mod_gmute:              // Andy case; printf("gmute\n");
        if (state)
            set_mode_group_mute();
        else
            unset_mode_group_mute();
        break;

    case c_midi_control_mod_glearn:             // Andy case; printf("glearn\n");
        if (state)
            set_mode_group_learn();
        else
            unset_mode_group_learn();
        break;

    case c_midi_control_play_ss:                // Andy case; printf("play_ss\n");
        set_playing_screenset();
        break;

    default:

        /*
         * Based on the value of c_midi_track_crl (32 *2) versus
         * c_seqs_in_set (32), maybe the first comparison should be
         * "a_control >= 2 * c_seqs_in_set".
         */

        if ((a_control >= c_seqs_in_set) && (a_control < c_midi_track_ctrl))
            select_and_mute_group(a_control - c_seqs_in_set);

        break;
    }
}

/**
 *
 *  This function is called by input_thread_func().
 */

void
perform::input_func ()
{
    event ev;
    while (m_inputing)
    {
        if (m_master_bus.poll_for_midi() > 0)
        {
            do
            {
                if (m_master_bus.get_midi_event(&ev))
                {
                    if (ev.get_status() == EVENT_MIDI_START)
                    {
                        // Obey MidiTimeClock.

                        stop();
                        start(false);
                        m_midiclockrunning = true;
                        m_usemidiclock = true;
                        m_midiclocktick = 0;
                        m_midiclockpos = 0;
                    }
                    else if (ev.get_status() == EVENT_MIDI_CONTINUE)
                    {
                        // MIDI continue: start from current position.

                        m_midiclockrunning = true;
                        start(false);
                    }
                    else if (ev.get_status() == EVENT_MIDI_STOP)
                    {
                        /*
                         * Do nothing, just let the system pause.  Since
                         * we're not getting ticks after the stop, the
                         * song won't advance when start is received,
                         * we'll reset the position, or. when continue is
                         * received, we won't reset the position.
                         */

                        m_midiclockrunning = false;
                        all_notes_off();
                    }
                    else if (ev.get_status() == EVENT_MIDI_CLOCK)
                    {
                        if (m_midiclockrunning)
                            m_midiclocktick += 8;
                    }
                    else if (ev.get_status() == EVENT_MIDI_SONG_POS)
                    {
                        // not tested (todo: test it!)

                        unsigned char a, b;
                        ev.get_data(a, b);
                        m_midiclockpos = (int(a) << 7) && int(b);
                    }

                    /*
                     * Filter system-wide messages.
                     */

                    if (ev.get_status() <= EVENT_SYSEX)
                    {
                        if (rc().show_midi())
                            ev.print();

                        if (m_master_bus.is_dumping())
                        {
                            // If a sequence is set, dump to it

                            ev.set_timestamp(m_tick);
                            m_master_bus.get_sequence()->stream_event(&ev);
                        }
                        else
                        {
                            // use it to control our sequencer

                            for (int i = 0; i < c_midi_controls; i++)
                            {
                                unsigned char data[2] = { 0, 0 };
                                unsigned char status = ev.get_status();
                                ev.get_data(data[0], data[1]);
                                if
                                (
                            get_midi_control_toggle(i)->m_active &&
                            status  == get_midi_control_toggle(i)->m_status &&
                            data[0] == get_midi_control_toggle(i)->m_data
                                )
                                {
                                    if
                                    (
                            data[1] >= get_midi_control_toggle(i)->m_min_value &&
                            data[1] <= get_midi_control_toggle(i)->m_max_value
                                    )
                                    {
                                        if (i <  c_seqs_in_set)
                                            sequence_playing_toggle(i+m_offset);
                                    }
                                }
                                if
                                (
                            get_midi_control_on(i)->m_active &&
                            status  == get_midi_control_on(i)->m_status &&
                            data[0] == get_midi_control_on(i)->m_data
                                )
                                {
                                    if
                                    (
                            data[1] >= get_midi_control_on(i)->m_min_value &&
                            data[1] <= get_midi_control_on(i)->m_max_value
                                    )
                                    {
                                        if (i <  c_seqs_in_set)
                                            sequence_playing_on(i + m_offset);
                                        else
                                            handle_midi_control(i, true);
                                    }
                                    else if (get_midi_control_on(i)->m_inverse_active)
                                    {
                                        if (i <  c_seqs_in_set)
                                            sequence_playing_off(i + m_offset);
                                        else
                                            handle_midi_control(i, false);
                                    }
                                }

                                if
                                (
                            get_midi_control_off(i)->m_active &&
                            status  == get_midi_control_off(i)->m_status &&
                            data[0] == get_midi_control_off(i)->m_data
                                )
                                {
                                    if
                                    (
                            data[1] >= get_midi_control_off(i)->m_min_value &&
                            data[1] <= get_midi_control_off(i)->m_max_value
                                    )
                                    {
                                        if (i <  c_seqs_in_set)
                                            sequence_playing_off(i + m_offset);
                                        else
                                            handle_midi_control(i, false);
                                    }
                                    else if (get_midi_control_off(i)->m_inverse_active)
                                    {
                                        if (i <  c_seqs_in_set)
                                            sequence_playing_on(i + m_offset);
                                        else
                                            handle_midi_control(i, true);
                                    }
                                }
                            }
                        }
                    }
                    if (ev.get_status() == EVENT_SYSEX)
                    {
                        if (rc().show_midi())
                            ev.print();

                        if (rc().pass_sysex())
                            m_master_bus.sysex(&ev);
                    }
                }
            } while (m_master_bus.is_more_input());
        }
    }
    pthread_exit(0);
}

/**
 *  For all active patterns/sequences, this function gets the playing
 *  status and saves it in m_sequence_state[i].
 */

void
perform::save_playing_state ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "save_playing_state"))
                m_sequence_state[i] = m_seqs[i]->get_playing();
        }
        else
            m_sequence_state[i] = false;
    }
}

/**
 *  For all active patterns/sequences, this function gets the playing
 *  status from m_sequence_state[i] and sets it for the sequence.
 */

void
perform::restore_playing_state ()
{
    for (int i = 0; i < m_sequence_max; i++)
    {
        if (is_active(i))
        {
            if (not_nullptr_assert(m_seqs[i], "restore_playing_state"))
                m_seqs[i]->set_playing(m_sequence_state[i]);
        }
    }
}

/**
 *  If the given status is present in the c_status_snapshot, the playing
 *  state is saved.  Then the given status is OR'd into the
 *  m_control_status.
 */

void
perform::set_sequence_control_status (int a_status)
{
    if (a_status & c_status_snapshot)
    {
        save_playing_state();
    }
    m_control_status |= a_status;
}

/**
 *  If the given status is present in the c_status_snapshot, the playing
 *  state is restored.  Then the given status is reversed in
 *  m_control_status.
 */

void
perform::unset_sequence_control_status (int a_status)
{
    if (a_status & c_status_snapshot)
    {
        restore_playing_state();
    }
    m_control_status &= (~a_status);
}

/**
 *
 */

void
perform::sequence_playing_toggle (int sequence)
{
    if (is_active(sequence))
    {
        if (not_nullptr_assert(m_seqs[sequence], "sequence_playing_toggle"))
        {
            if (m_control_status & c_status_queue)
            {
                m_seqs[sequence]->toggle_queued();
            }
            else
            {
                if (m_control_status & c_status_replace)
                {
                    unset_sequence_control_status(c_status_replace);
                    off_sequences();
                }
                m_seqs[sequence]->toggle_playing();
            }
        }
    }
}

/**
 *
 */

void
perform::sequence_playing_on (int seq)
{
    if (is_active(seq))
    {
        int next_offset = m_playscreen_offset + c_seqs_in_set;
        if
        (
            m_mode_group && (m_playing_screen == m_screenset) &&
            (seq >= m_playscreen_offset) && (seq < next_offset)
        )
        {
            m_tracks_mute_state[seq - m_playscreen_offset] = true;
        }
        if (not_nullptr_assert(m_seqs[seq], "sequence_playing_on"))
        {
            if (! m_seqs[seq]->get_playing())
            {
                if (m_control_status & c_status_queue)
                {
                    if (! m_seqs[seq]->get_queued())
                        m_seqs[seq]->toggle_queued();
                }
                else
                    m_seqs[seq]->set_playing(true);
            }
            else
            {
                if
                (   m_seqs[seq]->get_queued() &&
                    (m_control_status & c_status_queue)
                )
                {
                    m_seqs[seq]->toggle_queued();
                }
            }
        }
    }
}

/**
 *  Turn off the playing of a sequence, if it is active.
 *
 * \param seq
 *      The number of the seq to be turned off.
 */

void
perform::sequence_playing_off (int seq)
{
    if (is_active(seq))
    {
        int next_offset = m_playscreen_offset + c_seqs_in_set;
        if
        (
            m_mode_group && (m_playing_screen == m_screenset) &&
            (seq >= m_playscreen_offset) && (seq < next_offset)
        )
        {
            m_tracks_mute_state[seq-m_playing_screen * c_seqs_in_set] =
               false;
        }
        if (not_nullptr_assert(m_seqs[seq], "sequence_playing_off"))
        {
            bool queued = m_seqs[seq]->get_queued();
            if (m_seqs[seq]->get_playing())
            {
                if (m_control_status & c_status_queue)
                {
                    if (! queued)
                        m_seqs[seq]->toggle_queued();
                }
                else
                    m_seqs[seq]->set_playing(false);
            }
            else
            {
                if (queued && (m_control_status & c_status_queue))
                    m_seqs[seq]->toggle_queued();
            }
        }
    }
}

/**
 *  Pass-along function for keys().set_all_key_events.
 */

void
perform::set_all_key_events ()
{
    keys().set_all_key_events();
}

/**
 *  Pass-along function for keys().set_all_key_events.
 */

void
perform::set_all_key_groups ()
{
    keys().set_all_key_groups();
}

/**
 *  At construction time, this function sets up one keycode and one event
 *  slot.
 *
 *  It is called 32 times, corresponding to the pattern/sequence slots in
 *  the Patterns window.
 *
 *  It first removes the given key-code from the regular and reverse
 *  slot-maps.  Then it removes the sequence-slot from the regular and
 *  reverse slot-maps.
 *
 *  Finally, it adds the sequence-slot with a key value of key-code, and
 *  adds the key-code with a value of sequence-slot.
 *
 *  Why are we erasing four items instead of just two?
 */

void
perform::set_key_event (unsigned int keycode, long sequence_slot)
{
    keys().set_key_event(keycode, sequence_slot);
}

/**
 *  At construction time, this function sets up one keycode and one group
 *  slot.
 *
 *  It is called 32 times, corresponding the pattern/sequence slots in the
 *  Patterns window.
 *
 *  Compare it to the set_key_events() function.
 */

void
perform::set_key_group (unsigned int keycode, long group_slot)
{
    keys().set_key_group(keycode, group_slot);
}

/*
 * Non-inline encapsulation functions start here.
 */

/**
 *  Handle a sequence key to toggle the playing of an active pattern in
 *  the selected screen-set.
 */

void
perform::sequence_key (int seq)
{
    int offset = get_screenset() * c_mainwnd_rows * c_mainwnd_cols;
    if (is_active(seq + offset))
        sequence_playing_toggle(seq + offset);
}

/**
 *  Provides a way to format the sequence parameters string for display in the
 *  mainwid or perfnames modules.  This string goes on the bottom-left of
 *  those user-interface elements.
 *
 *  The format of this string is something like the following example,
 *  depending on the "show sequence numbers" option.  The values shown are, in
 *  this order, sequence number (if allowed), buss number, channel number,
 *  beats per bar, and beat width.
 *
\verbatim
        No sequence number:     31-16 4/4
        Sequence number:        9  31-16 4/4
\endverbatim
 *
 *  The sequence number and buss number are re 0, while the channel number is
 *  displayed re 1.
 *
 * \note
 *      Later, we could add the sequence hot-key to this string, though
 *      showing that is not much use in perfnames.  Also, this function is a
 *      stilted mix of direct access and access through sequence number.
 *
 * \param seq
 *      Provides the reference to the sequence, use for getting the sequence
 *      parameters to be written to the label string.
 *
 * \return
 *      Returns the filled in label if the sequence is active.
 *      Otherwise, an empty string is returned.
 */

std::string
perform::sequence_label (const sequence & seq)
{
    std::string result;
    int sn = seq.number();
    if (is_active(sn))
    {
        char tmp[32];
        int bus = seq.get_midi_bus();
        int chan = seq.get_midi_channel() + 1;
        int bpb = int(seq.get_beats_per_bar());
        int bw = int(seq.get_beat_width());
        if (show_ui_sequence_number())                  /* new feature! */
            snprintf(tmp, sizeof tmp, "%-3d%d-%d %d/%d", sn, bus, chan, bpb, bw);
        else
            snprintf(tmp, sizeof tmp, "%d-%d %d/%d", bus, chan, bpb, bw);

        result = std::string(tmp);
    }
    return result;
}

/**
 *  Sets the input bus, and handles the special "key labels on sequence" and
 *  "sequence numbers on sequence" functionality.  This function is called by
 *  options::input_callback().
 *
 * \tricky
 *      See the bus parameter.  We should provide two separate functions for
 *      this feature, but it is already combined into one input-callback
 *      function with a lot of other functionality in the options module.
 *
 * \param bus
 *      If this value is greater than SEQ64_DEFAULT_BUSS_MAX (32), then it is
 *      treated as a user-interface flag (PERFORM_KEY_LABELS_ON_SEQUENCE or
 *      PERFORM_NUM_LABELS_ON_SEQUENCE) that causes all the sequences to be
 *      dirtied, and thus get redrawn iwht the new user-interface setting.
 *
 * \param active
 *      Indicates whether the buss or the user-interface feature is active or
 *      inactive.
 */

void
perform::set_input_bus (int bus, bool active)
{
    if (bus >= SEQ64_DEFAULT_BUSS_MAX)                  /* 32 busses    */
    {
        if (bus == PERFORM_KEY_LABELS_ON_SEQUENCE)
            show_ui_sequence_key(active);
        else if (bus == PERFORM_NUM_LABELS_ON_SEQUENCE)
            show_ui_sequence_number(active);

        for (int seq = 0; seq < m_sequence_max; seq++)
        {
            sequence * s = get_sequence(seq);
            if (not_nullptr(s))
                s->set_dirty();
        }
    }
    else if (bus >= 0)
        master_bus().set_input(bus, active);
}

/**
 *  Provided for mainwnd::on_key_press_event() and
 *  mainwnd::on_key_release_event() to call.
 *
 * \return
 *      Returns true if the key was handled.
 */

bool
perform::mainwnd_key_event (const keystroke & k)
{
    bool result = true;
    unsigned int key = k.key();
    if (k.is_press())
    {
        if (key == keys().replace())
            set_sequence_control_status(c_status_replace);
        else if (key == keys().queue() || key == keys().keep_queue())
            set_sequence_control_status(c_status_queue);
        else if (key == keys().snapshot_1() || key == keys().snapshot_2())
            set_sequence_control_status(c_status_snapshot);
        else if (key == keys().set_playing_screenset())
            set_playing_screenset();
        else if (key == keys().group_on())
            set_mode_group_mute();
        else if (key == keys().group_off())
            unset_mode_group_mute();
        else if (key == keys().group_learn())
            set_mode_group_learn();
        else
            result = false;
    }
    else
    {
        if (key == keys().replace())
            unset_sequence_control_status(c_status_replace);
        else if (key == keys().queue())
            unset_sequence_control_status(c_status_queue);
        else if (key == keys().snapshot_1() || key == keys().snapshot_2())
            unset_sequence_control_status(c_status_snapshot);
        else if (key == keys().group_learn())
            unset_mode_group_learn();
        else
            result = false;
    }
    return result;
}

/**
 *  Provided for perfroll::on_key_press_event() and
 *  perfroll::on_key_release_event() to call.
 *
 * \return
 *      Returns true if the key was handled.
 */

bool
perform::perfroll_key_event (const keystroke & k, int drop_sequence)
{
    bool result = false;
    if (k.is_press())                       // a_p0->type == SEQ64_PRESS
    {
        if (is_active(drop_sequence))
        {
            if (k.is_delete())
            {
                push_trigger_undo();
                get_sequence(drop_sequence)->del_selected_trigger();
                result = true;
            }
            else if (k.mod_control())            // SEQ64_CONTROL_MASK
            {
                if (k.is_letter('x'))                           /* cut  */
                {
                    push_trigger_undo();
                    get_sequence(drop_sequence)->cut_selected_trigger();
                    result = true;
                }
                else if (k.is_letter('c'))                      /* copy     */
                {
                    get_sequence(drop_sequence)->copy_selected_trigger();
                    result = true;
                }
                else if (k.is_letter('v'))                      /* paste    */
                {
                    push_trigger_undo();
                    get_sequence(drop_sequence)->paste_trigger();
                    result = true;
                }
                else if (k.is_letter('z'))                      /* undo     */
                {
                    /*
                     * Can we support undo here?
                     *
                     * result = true;
                     */
                }
            }
        }
    }
    return result;
}

}           // namespace seq64

/*
 * perform.cpp
 *
 * vim: sw=4 ts=4 wm=4 et ft=cpp
 */
