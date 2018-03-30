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

#include "midi_control_out.hpp"

/*
 *  Do not document a namespace; it breaks Doxygen.
 */

namespace seq64
{

midi_control_out::midi_control_out (mastermidibus *mmbus, bussbyte buss)
    :
    m_mmbus(mmbus),
    m_buss(buss)
{
    event dummy_e;
    for (int i=0; i<32; ++i)
    {	
	for (int a=0; a<action_max; ++a)
	{
	    m_seq_event[i][a] = dummy_e;
	    m_seq_active[i][a] = false;
	}
    }
}

    
void midi_control_out::send_seq_event(int seq, action what)
{
    if (seq < 0 || seq >= 32)
    {
	return;
    } else {
	if (!m_seq_active[seq][what])
	{
	    return;
	}
	event ev = m_seq_event[seq][what];
	m_mmbus->play(m_buss, &ev, ev.get_channel());
    }
}

event midi_control_out::get_seq_event(int seq, action what) const
{
    if (seq < 0 || seq >= 32)
    {
	event dummy_event;
	return dummy_event;
    } else {
	return m_seq_event[seq][what];
    }
}   
    
void midi_control_out::set_seq_event(int seq, action what, event& ev)
{
    m_seq_event[seq][what] = ev;
    m_seq_active[seq][what] = true;
}
    

bool midi_control_out::seq_event_is_active(int seq, action what) const
{
    if (seq < 0 || seq >= 32)
    {
	return false;
    } else {
	return m_seq_active[seq][what];
    }
}

    
    
}
