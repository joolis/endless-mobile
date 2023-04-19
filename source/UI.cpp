/* UI.cpp
Copyright (c) 2014 by Michael Zahniser

Endless Sky is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later version.

Endless Sky is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "UI.h"

#include "Command.h"
#include "Gesture.h"
#include "Panel.h"
#include "Screen.h"

#include <SDL2/SDL.h>

#include <SDL2/SDL_log.h>
#include <algorithm>

using namespace std;



// Handle an event. The event is handed to each panel on the stack until one
// of them handles it. If none do, this returns false.
bool UI::Handle(const SDL_Event &event)
{
	bool handled = false;

	vector<shared_ptr<Panel>>::iterator it = stack.end();
	while(it != stack.begin() && !handled)
	{
		--it;
		// Panels that are about to be popped cannot handle any other events.
		if(count(toPop.begin(), toPop.end(), it->get()))
			continue;

		if(event.type == SDL_MOUSEMOTION)
		{
			if(event.motion.state & SDL_BUTTON(1))
				handled = (*it)->Drag(
					event.motion.xrel * 100. / Screen::Zoom(),
					event.motion.yrel * 100. / Screen::Zoom());
			else
				handled = (*it)->Hover(
					Screen::Left() + event.motion.x * 100 / Screen::Zoom(),
					Screen::Top() + event.motion.y * 100 / Screen::Zoom());
		}
		else if(event.type == SDL_MOUSEBUTTONDOWN)
		{
			int x = Screen::Left() + event.button.x * 100 / Screen::Zoom();
			int y = Screen::Top() + event.button.y * 100 / Screen::Zoom();
			if(event.button.button == 1)
			{
				handled = (*it)->ZoneMouseDown(Point(x, y));
				if(!handled)
					handled = (*it)->Click(x, y, event.button.clicks);
			}
			else if(event.button.button == 3)
				handled = (*it)->RClick(x, y);
		}
		else if(event.type == SDL_MOUSEBUTTONUP)
		{
			int x = Screen::Left() + event.button.x * 100 / Screen::Zoom();
			int y = Screen::Top() + event.button.y * 100 / Screen::Zoom();
			handled = (*it)->ZoneMouseUp(Point(x, y));
			if(!handled)
				handled = (*it)->Release(x, y);
		}
		else if(event.type == SDL_MOUSEWHEEL)
			handled = (*it)->Scroll(event.wheel.x, event.wheel.y);
		else if(event.type == SDL_FINGERDOWN)
		{
			// finger coordinates are 0 to 1, normalize to screen coordinates
			int x = (event.tfinger.x - .5) * Screen::Width();
			int y = (event.tfinger.y - .5) * Screen::Height();

			// Order:
			//   1. Zones (these will be buttons)
			//   2. Finger down events (this will be game controls)
			//      2.5 Trigger a hover as well, as some ui's use this to
			//          determine where a drag begins from.
			//   3. Clicks (fallback to mouse click)
			if(!handled)
			{
				if((handled = (*it)->ZoneMouseDown(Point(x, y))))
					zoneFingerId = event.tfinger.fingerId;
			}
			if(!handled)
			{
				(*it)->Hover(x, y);
				handled = (*it)->FingerDown(x, y, event.tfinger.fingerId);
			}
			if(!handled)
			{
				uint32_t now = SDL_GetTicks();
				if((handled = (*it)->Click(x, y, (now - lastTap) > 500 ? 1 : 2)))
					panelFingerId = event.tfinger.fingerId;
				lastTap = now;
			}
		}
		else if(event.type == SDL_FINGERMOTION)
		{
			// finger coordinates are 0 to 1, normalize to screen coordinates
			int x = (event.tfinger.x - .5) * Screen::Width();
			int y = (event.tfinger.y - .5) * Screen::Height();
			int dx = (event.tfinger.dx) * Screen::Width();
			int dy = (event.tfinger.dy) * Screen::Height();

			// Order:
			//   1. FingerMove events (These will be game controls)
			//   2. Drag (ui events)

			if(!handled)
				handled = (*it)->FingerMove(x, y, event.tfinger.fingerId);
			if(!handled && panelFingerId == event.tfinger.fingerId)
				handled = (*it)->Drag(dx, dy);
		}
		else if(event.type == SDL_FINGERUP)
		{
			// finger coordinates are 0 to 1, normalize to screen coordinates
			int x = (event.tfinger.x - .5) * Screen::Width();
			int y = (event.tfinger.y - .5) * Screen::Height();

			// Order:
			//   1. Zones (these will be buttons)
			//   2. Finger down events (this will be game controls)
			//   3. Clicks (fallback to mouse click)
			if(!handled && zoneFingerId == event.tfinger.fingerId)
			{
				handled = (*it)->ZoneMouseUp(Point(x, y));
				zoneFingerId = -1;
			}
			if(!handled)
				handled = (*it)->FingerUp(x, y, event.tfinger.fingerId);
			if(!handled && panelFingerId == event.tfinger.fingerId)
			{
				handled = (*it)->Release(x, y);
				panelFingerId = -1;
			}
		}
		else if(event.type == SDL_KEYDOWN)
		{
			Command command(event.key.keysym.sym);
			handled = (*it)->KeyDown(event.key.keysym.sym, event.key.keysym.mod, command, !event.key.repeat);
		}
		else if(event.type == Command::EventID())
		{
			Command command(event);
			if(event.button.state == SDL_PRESSED)
				handled = (*it)->KeyDown(0, 0, command, true);
		}
		else if(event.type == Gesture::EventID())
		{
			auto gesture_type = static_cast<Gesture::GestureEnum>(event.user.code);

			// if the panel doesn't want the gesture, convert it to a
			// command, and try again
			if(!(handled = (*it)->Gesture(gesture_type)))
			{
				Command command(gesture_type);
				Command::InjectOnce(command);
				handled = (*it)->KeyDown(0, 0, command, true);
			}
		}

		// If this panel does not want anything below it to receive events, do
		// not let this event trickle further down the stack.
		if((*it)->TrapAllEvents())
			break;
	}

	// Handle any queued push or pop commands.
	PushOrPop();

	return handled;
}



// Step all the panels forward (advance animations, move objects, etc.).
void UI::StepAll()
{
	// Handle any queued push or pop commands.
	PushOrPop();

	// Step all the panels.
	for(shared_ptr<Panel> &panel : stack)
		panel->Step();
}



// Draw all the panels.
void UI::DrawAll()
{
	// First, clear all the clickable zones. New ones will be added in the
	// course of drawing the screen.
	for(const shared_ptr<Panel> &it : stack)
		it->ClearZones();

	// Find the topmost full-screen panel. Nothing below that needs to be drawn.
	vector<shared_ptr<Panel>>::const_iterator it = stack.end();
	while(it != stack.begin())
		if((*--it)->IsFullScreen())
			break;

	for( ; it != stack.end(); ++it)
		(*it)->Draw();
}



// Add the given panel to the stack. UI is responsible for deleting it.
void UI::Push(Panel *panel)
{
	Push(shared_ptr<Panel>(panel));
}



void UI::Push(const shared_ptr<Panel> &panel)
{
	panel->SetUI(this);
	toPush.push_back(panel);
}



// Remove the given panel from the stack (if it is in it). The panel will be
// deleted at the start of the next time Step() is called, so it is safe for
// a panel to Pop() itself.
void UI::Pop(const Panel *panel)
{
	toPop.push_back(panel);
}



// Remove the given panel and every panel that is higher in the stack.
void UI::PopThrough(const Panel *panel)
{
	for(auto it = stack.rbegin(); it != stack.rend(); ++it)
	{
		toPop.push_back(it->get());
		if(it->get() == panel)
			break;
	}
}



// Check whether the given panel is on top of the existing panels, i.e. is the
// active one, on this Step. Any panels that have been pushed this Step are not
// considered.
bool UI::IsTop(const Panel *panel) const
{
	return (!stack.empty() && stack.back().get() == panel);
}



// Get the absolute top panel, even if it is not yet drawn (i.e. was pushed on
// this Step).
shared_ptr<Panel> UI::Top() const
{
	if(!toPush.empty())
		return toPush.back();

	if(!stack.empty())
		return stack.back();

	return shared_ptr<Panel>();
}



// Delete all the panels and clear the "done" flag.
void UI::Reset()
{
	stack.clear();
	toPush.clear();
	toPop.clear();
	isDone = false;
}



// Get the lower-most panel.
shared_ptr<Panel> UI::Root() const
{
	if(stack.empty())
	{
		if(toPush.empty())
			return shared_ptr<Panel>();

		return toPush.front();
	}

	return stack.front();
}



// If the player enters the game, enable saving the loaded file.
void UI::CanSave(bool canSave)
{
	this->canSave = canSave;
}



bool UI::CanSave() const
{
	return canSave;
}



// Tell the UI to quit.
void UI::Quit()
{
	isDone = true;
}



// Check if it is time to quit.
bool UI::IsDone() const
{
	return isDone;
}



// Check if there are no panels left. No panels left on the gamePanels-
// stack usually means that it is time for the game to quit, while no
// panels left on the menuPanels-stack is a normal state for a running
// game.
bool UI::IsEmpty() const
{
	return stack.empty() && toPush.empty();
}



// Get the current mouse position.
Point UI::GetMouse()
{
	int x = 0;
	int y = 0;
	SDL_GetMouseState(&x, &y);
	return Screen::TopLeft() + Point(x, y) * (100. / Screen::Zoom());
}



// If a push or pop is queued, apply it.
void UI::PushOrPop()
{
	// Handle any panels that should be added.
	for(shared_ptr<Panel> &panel : toPush)
		if(panel)
			stack.push_back(panel);
	toPush.clear();

	// These panels should be popped but not deleted (because someone else
	// owns them and is managing their creation and deletion).
	for(const Panel *panel : toPop)
	{
		for(auto it = stack.begin(); it != stack.end(); ++it)
			if(it->get() == panel)
			{
				it = stack.erase(it);
				break;
			}
	}
	toPop.clear();
}
