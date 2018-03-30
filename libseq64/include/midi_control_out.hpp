#ifndef SEQ64_MIDI_CONTROL_OUT_HPP
#define SEQ64_MIDI_CONTROL_OUT_HPP

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
 * \file          midi_control_out.hpp
 *
 *  This module declares/defines the class for handling MIDI control
 *  <i>output</i> of the application.
 *
 * \library       sequencer64 application
 * \author        Igor Angst
 * \date          2018-03-28
 * \updates       
 * \license       GNU GPLv2 or above
 *
 * The class contained in this file encapsulates most of the
 * functionality to send feedback to an external control surface in
 * order to reflect the state of sequencer64. This includes updates on
 * the playing and queueing status of the sequences. 
 *
 */

#include "globals.h"
#include "mastermidibus.hpp"
#include "event.hpp"

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq64
{


    
class midi_control_out
{
public:

    /**
     *  Provides the kind of MIDI control event that is sent out.
     *
     * \var action_play
     *      Sequence is playing.
     * 
     * \var action_mute
     *      Sequence is muted.
     *
     * \var action_queue
     *      Sequence is queued.
     *
     * \var action_unqueue
     *      Sequence is unqueued.
     * 
     * \var action_activate
     *      Sequence is loaded or created.
     *
     * \var action_delete
     *      Sequence is deleted from its slot.
     */

    typedef enum
    {
	action_play     = 0,
	action_mute     = 1,
	action_queue    = 2,
	action_unqueue  = 3,
	action_activate = 4,
	action_delete   = 5,
	action_max      = 6
    } action;
    
private:

    /**
     *  Provides the MIDI output master bus.
     */
    mastermidibus* m_mmbus;

    /**
     *  Provides the MIDI output buss.
     */
    bussbyte m_buss;

    /**
     *  Provides the events to be sent out for sequence status changes.
     */
    event m_seq_event[32][action_max];
    bool  m_seq_active[32][action_max];

public:

    midi_control_out(mastermidibus *mmbus, bussbyte buss);
    
    /** 
     * Send out notification about playing status of a sequence.
     *
     * \param seq
     *      The index of the sequence
     *
     * \param what
     *      The status action of the sequence 
     */
    void send_seq_event(int seq, action what);

    /** 
     * Getter for sequence action events.
     *
     * \param seq
     *      The index of the sequence.
     *
     * \param what
     *      The action to be notified.
     *
     * \returns
     *      The MIDI event to be sent.
     */
    event get_seq_event(int seq, action what) const;

    /** 
     * Register a MIDI event for a given sequence action.
     *
     * \param seq
     *      The index of the sequence.
     *
     * \param what
     *      The action to be notified.
     *
     * \param event
     *      The MIDI event to be sent.
     */
    void set_seq_event(int seq, action what, event& ev);

    /** 
     * Checks if a sequence status event is active.
     * 
     * \param seq
     *      The index of the sequence.
     *
     * \param what
     *      The action to be notified.
     *
     * \return 
     *      Returns true if the respective event is active.
     */
    bool seq_event_is_active(int seq, action what) const;
};

}

#endif
