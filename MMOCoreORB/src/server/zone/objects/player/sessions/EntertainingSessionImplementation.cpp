/*
 * EntertainingSessionImplementation.cpp
 *
 *  Created on: 20/08/2010
 *      Author: victor
 */

#include "server/zone/objects/player/sessions/EntertainingSession.h"
#include "server/zone/managers/skill/SkillManager.h"
#include "server/zone/managers/skill/Performance.h"
#include "server/zone/managers/skill/PerformanceManager.h"
#include "server/zone/managers/player/PlayerManager.h"
#include "server/zone/objects/group/GroupObject.h"
#include "server/zone/objects/creature/CreatureObject.h"
#include "server/zone/objects/player/events/EntertainingSessionTask.h"
#include "server/zone/objects/player/EntertainingObserver.h"
#include "templates/params/creature/CreatureAttribute.h"
#include "server/zone/objects/player/PlayerObject.h"
#include "server/zone/objects/player/FactionStatus.h"
#include "server/zone/objects/tangible/Instrument.h"
#include "server/zone/packets/object/Flourish.h"
#include "server/zone/packets/creature/CreatureObjectDeltaMessage6.h"
#include "server/zone/objects/mission/MissionObject.h"
#include "server/zone/objects/mission/EntertainerMissionObjective.h"
#include "server/zone/objects/creature/buffs/PerformanceBuff.h"
#include "server/zone/objects/creature/buffs/PerformanceBuffType.h"
#include "server/zone/objects/mission/MissionTypes.h"
#include "server/zone/objects/building/BuildingObject.h"
#include "server/chat/ChatManager.h"

void EntertainingSessionImplementation::doEntertainerPatronEffects() {
	ManagedReference<CreatureObject*> creo = entertainer.get();

	if (creo == nullptr)
		return;

	if (performanceName == "")
		return;

	Locker locker(creo);

	//**DECLARATIONS**
	VectorMap<ManagedReference<CreatureObject*>, EntertainingData>* patrons = nullptr;

	SkillManager* skillManager = creo->getZoneServer()->getSkillManager();

	PerformanceManager* performanceManager = skillManager->getPerformanceManager();
	Performance* performance = nullptr;

	ManagedReference<Instrument*> instrument = getInstrument(creo);

	float woundHealingSkill = 0.0f;
	float playerShockHealingSkill = 0.0f;
	float buildingShockHealingSkill = creo->getSkillMod("private_med_battle_fatigue");
	float factionPerkSkill = creo->getSkillMod("private_faction_mind_heal");

	//**LOAD PATRONS, GET THE PERFORMANCE AND ENT'S HEALING SKILL.**
	if (dancing) {
		patrons = &watchers;
		performance = performanceManager->getDance(performanceName);
		woundHealingSkill = (float) creo->getSkillMod("healing_dance_wound");
		playerShockHealingSkill = (float) creo->getSkillMod("healing_dance_shock");
	} else if (playingMusic && instrument != nullptr) {
		patrons = &listeners;
		performance = performanceManager->getSong(performanceName, instrument->getInstrumentType());
		woundHealingSkill = (float) creo->getSkillMod("healing_music_wound");
		playerShockHealingSkill = (float) creo->getSkillMod("healing_music_shock");

	} else {
		cancelSession();
		return;
	}

	if (performance == nullptr) {
		return;
	}

	ManagedReference<BuildingObject*> building = cast<BuildingObject*>(creo->getRootParent());

	if (building != nullptr && factionPerkSkill > 0 && building->isPlayerRegisteredWithin(creo->getObjectID())) {
		unsigned int buildingFaction = building->getFaction();
		unsigned int healerFaction = creo->getFaction();

		if (healerFaction != 0 && healerFaction == buildingFaction && creo->getFactionStatus() == FactionStatus::OVERT) {
			woundHealingSkill += factionPerkSkill;
			playerShockHealingSkill += factionPerkSkill;
		}
	}

	//**DETERMINE WOUND HEAL AMOUNTS.**
	int woundHeal = ceil(performance->getHealMindWound() * (woundHealingSkill / 100.0f));
	int shockHeal = ceil(performance->getHealShockWound() * ((playerShockHealingSkill + buildingShockHealingSkill) / 100.0f));

	//**ENTERTAINER HEALS THEIR OWN MIND.**
	healWounds(creo, woundHeal*(flourishCount+1), shockHeal*(flourishCount+1));

	//**APPLY EFFECTS TO PATRONS.**
	if (patrons != nullptr && patrons->size() > 0) {

		for (int i = 0; i < patrons->size(); ++i) {
			ManagedReference<CreatureObject*> patron = patrons->elementAt(i).getKey();

			try {
				//**VERIFY THE PATRON IS NOT ON THE DENY SERVICE LIST

				if (creo->isInRange(patron, 10.0f)) {
					healWounds(patron, woundHeal*(flourishCount+1), shockHeal*(flourishCount+1));
					increaseEntertainerBuff(patron);

				} else { //patron is not in range, force to stop listening
					ManagedReference<PlayerManager*> playerManager = patron->getZoneServer()->getPlayerManager();

					Locker locker(patron, creo);

					if (dancing) {
						if (playerManager != nullptr)
							playerManager->stopWatch(patron, creo->getObjectID(), true, false, false, true);

						if (!patron->isListening())
							sendEntertainmentUpdate(patron, 0, "", true);

					} else if (playingMusic) {
						if (playerManager != nullptr)
							playerManager->stopListen(patron, creo->getObjectID(), true, false, false, true);

						if (!patron->isWatching())
							sendEntertainmentUpdate(patron, 0, "", true);
					}
				}

			} catch (Exception& e) {
				error("Unreported exception caught in EntertainingSessionImplementation::doEntertainerPatronEffects()");
			}
		}
	} //else
	//System::out << "There are no patrons.\n";


	info("EntertainingSessionImplementation::doEntertainerPatronEffects() end");
}

bool EntertainingSessionImplementation::isInEntertainingBuilding(CreatureObject* creature) {
	ManagedReference<SceneObject*> root = creature->getRootParent();

	if (root != nullptr) {
		uint32 gameObjectType = root->getGameObjectType();

		switch (gameObjectType) {
		case SceneObjectType::RECREATIONBUILDING:
		case SceneObjectType::HOTELBUILDING:
		case SceneObjectType::THEATERBUILDING:
			return true;
		}
	}

	return false;
}

void EntertainingSessionImplementation::healWounds(CreatureObject* creature, float woundHeal, float shockHeal) {
	float amountHealed = 0;

	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	Locker clocker(creature, entertainer);

	if(!canGiveEntertainBuff())
		return;

	if(isInDenyServiceList(creature))
		return;

	if(shockHeal > 0 && creature->getShockWounds() > 0 && canHealBattleFatigue()) {
		creature->addShockWounds(-shockHeal, true, false);
		amountHealed += shockHeal;
	}
	if(woundHeal > 0 && (creature->getWounds(CreatureAttribute::MIND) > 0
			|| creature->getWounds(CreatureAttribute::FOCUS) > 0
			|| creature->getWounds(CreatureAttribute::WILLPOWER) > 0)) {
		creature->healWound(entertainer, CreatureAttribute::MIND, woundHeal, true, false);
		creature->healWound(entertainer, CreatureAttribute::FOCUS, woundHeal, true, false);
		creature->healWound(entertainer, CreatureAttribute::WILLPOWER, woundHeal, true, false);

		amountHealed += woundHeal;
	}

	clocker.release();

	if(entertainer->getGroup() != nullptr)
		addHealingXpGroup(amountHealed);
	else
		addHealingXp(amountHealed);

}

void EntertainingSessionImplementation::addHealingXpGroup(int xp) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	ManagedReference<GroupObject*> group = entertainer->getGroup();
	ManagedReference<PlayerManager*> playerManager = entertainer->getZoneServer()->getPlayerManager();

	// Apply 10x Entertainer XP multiplier
	xp *= 10;

	for (int i = 0; i < group->getGroupSize(); ++i) {
		try {
			ManagedReference<CreatureObject *> groupMember = group->getGroupMember(i);

			if (groupMember != nullptr && groupMember->isPlayerCreature()) {
				Locker clocker(groupMember, entertainer);

				if (groupMember->isEntertaining() && groupMember->isInRange(entertainer, 40.0f)
					&& groupMember->hasSkill("social_entertainer_novice")) {
					String healxptype("entertainer_healing");

					if (playerManager != nullptr)
						playerManager->awardExperience(groupMember, healxptype, xp, true);
				}
			}
		} catch (Exception& e) {
			warning("exception in EntertainingSessionImplementation::addHealingXpGroup: " + e.getMessage());
		}
	}
}

void EntertainingSessionImplementation::activateAction() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	if (entertainer == nullptr)
		return;

	Locker locker(entertainer);

	if (!isDancing() && !isPlayingMusic()) {
		return; // don't tick action if they aren't doing anything
	}

	doEntertainerPatronEffects();
	awardEntertainerExperience();
	doPerformanceAction();


	startTickTask();

	// entertainer->info("EntertainerEvent completed.");
}

void EntertainingSessionImplementation::startTickTask() {
	if (tickTask == nullptr) {
		tickTask = new EntertainingSessionTask(_this.getReferenceUnsafeStaticCast());
	}

	if (!tickTask->isScheduled()) {
		tickTask->schedule(10000);
	}
}

void EntertainingSessionImplementation::doPerformanceAction() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	if (entertainer == nullptr)
		return;

	Locker locker(entertainer);

	Performance* performance = nullptr;

	PerformanceManager* performanceManager = SkillManager::instance()->getPerformanceManager();
	ManagedReference<Instrument*> instrument = getInstrument(entertainer);

	if (isDancing())
		performance = performanceManager->getDance(performanceName);
	else if (isPlayingMusic() && instrument)
		performance = performanceManager->getSong(performanceName, instrument->getInstrumentType());
	else {
		cancelSession();
		return;
	}

	if (performance == nullptr) { // shouldn't happen
		StringBuffer msg;
		msg << "Performance was null.  Please report to www.swgemu.com/bugs ! Name: " << performanceName << " and Type: " << dec << instrument->getInstrumentType();

		entertainer->sendSystemMessage(msg.toString());
		return;
	}

	int actionDrain = performance->getActionPointsPerLoop() - (int)(entertainer->getHAM(CreatureAttribute::QUICKNESS)/20.f);

	if (entertainer->getHAM(CreatureAttribute::ACTION) <= actionDrain) {
		if (isDancing()) {
			stopDancing();
			entertainer->sendSystemMessage("@performance:dance_too_tired");
		}

		if (isPlayingMusic()) {
			stopPlayingMusic();
			entertainer->sendSystemMessage("@performance:music_too_tired");
		}
	} else {
		entertainer->inflictDamage(entertainer, CreatureAttribute::ACTION, actionDrain, false, true);
	}
}

Instrument* EntertainingSessionImplementation::getInstrument(CreatureObject* creature) {
	//all equipable instruments are in hold_r

	if (targetInstrument) {
		ManagedReference<SceneObject*> target = creature->getZoneServer()->getObject(creature->getTargetID());

		if (target == nullptr)
			return nullptr;

		Instrument* instrument = dynamic_cast<Instrument*>(target.get());

		if (externalInstrument != nullptr && externalInstrument != instrument)
			return nullptr;
		else
			return instrument;
	} else {
		SceneObject* object = creature->getSlottedObject("hold_r");

		return dynamic_cast<Instrument*>(object);
	}
}

void EntertainingSessionImplementation::stopPlayingMusic() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	Locker locker(entertainer);

	if (!playingMusic)
		return;

	playingMusic = false;
	entertainer->sendSystemMessage("@performance:music_stop_self");

	sendEntertainingUpdate(entertainer, 0.8025000095f, entertainer->getPerformanceAnimation(), 0, 0);

	performanceName = "";
	entertainer->setListenToID(0);

	if (entertainer->getPosture() == CreaturePosture::SKILLANIMATING)
		entertainer->setPosture(CreaturePosture::UPRIGHT);

	if (externalInstrument != nullptr && externalInstrument->isBeingUsed())
		externalInstrument->setBeingUsed(false);

	externalInstrument = nullptr;

	ManagedReference<PlayerManager*> playerManager = entertainer->getZoneServer()->getPlayerManager();

	while (listeners.size() > 0) {
		ManagedReference<CreatureObject*> listener = listeners.elementAt(0).getKey();

		Locker clocker(listener, entertainer);

		playerManager->stopListen(listener, entertainer->getObjectID(), true, true, false);

		if (!listener->isWatching())
			sendEntertainmentUpdate(listener, 0, "", true);

		listeners.drop(listener);
	}

	if (tickTask != nullptr && tickTask->isScheduled())
		tickTask->cancel();

	targetInstrument = false;
	updateEntertainerMissionStatus(false, MissionTypes::MUSICIAN);

	entertainer->notifyObservers(ObserverEventType::STOPENTERTAIN, entertainer);

	entertainer->dropObserver(ObserverEventType::POSTURECHANGED, observer);

	ManagedReference<GroupObject*> group = entertainer->getGroup();

	if (group != nullptr) {
		bool otherPlaying = group->isOtherMemberPlayingMusic(entertainer);

		if (!otherPlaying) {
			Locker locker(group);

			group->setBandSong("");
		}
	}

	if (!dancing && !playingMusic) {
		ManagedReference<PlayerObject*> entPlayer = entertainer->getPlayerObject();
		if (entPlayer != nullptr && entPlayer->getPerformanceBuffTarget() != 0)
			entPlayer->setPerformanceBuffTarget(0);

		entertainer->dropActiveSession(SessionFacadeType::ENTERTAINING);
	}
}

void EntertainingSessionImplementation::startDancing(const String& dance, const String& animation) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	Locker locker(entertainer);

	sendEntertainingUpdate(entertainer, /*0x3C4CCCCD*/0.0125f, animation, 0x07339FF8, 0xDD);
	performanceName = dance;
	dancing = true;

	entertainer->sendSystemMessage("@performance:dance_start_self");

	updateEntertainerMissionStatus(true, MissionTypes::DANCER);

	entertainer->notifyObservers(ObserverEventType::STARTENTERTAIN, entertainer);

	startEntertaining();
}

void EntertainingSessionImplementation::startPlayingMusic(const String& song, const String& instrumentAnimation, int instrid) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	Locker locker(entertainer);

	ManagedReference<GroupObject*> group = entertainer->getGroup();

	sendEntertainingUpdate(entertainer, 0.0125f, instrumentAnimation, 0x07352BAC, instrid);
	performanceName = song;
	playingMusic = true;

	entertainer->sendSystemMessage("@performance:music_start_self");

	entertainer->setListenToID(entertainer->getObjectID(), true);

	externalInstrument = getInstrument(entertainer);

	if (externalInstrument != nullptr)
		externalInstrument->setBeingUsed(true);

	updateEntertainerMissionStatus(true, MissionTypes::MUSICIAN);

	entertainer->notifyObservers(ObserverEventType::STARTENTERTAIN, entertainer);

	startEntertaining();

	if (group != nullptr) {
		Locker clocker(group, entertainer);

		if (group->getBandSong() != song) {
			group->setBandSong(song);
		}
	}
}

void EntertainingSessionImplementation::startEntertaining() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	Locker locker(entertainer);

	entertainer->setPosture(CreaturePosture::SKILLANIMATING);

	startTickTask();

	if (observer == nullptr) {
		observer = new EntertainingObserver();
		observer->deploy();
	}

	entertainer->registerObserver(ObserverEventType::POSTURECHANGED, observer);
}

void EntertainingSessionImplementation::stopDancing() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	if (entertainer == nullptr)
		return;

	Locker locker(entertainer);

	if (!dancing)
		return;

	dancing = false;

	entertainer->sendSystemMessage("@performance:dance_stop_self");

	performanceName = "";

	sendEntertainingUpdate(entertainer, 0.8025000095f, entertainer->getPerformanceAnimation(), 0, 0);

	if (entertainer->getPosture() == CreaturePosture::SKILLANIMATING)
		entertainer->setPosture(CreaturePosture::UPRIGHT);


	ManagedReference<PlayerManager*> playerManager = entertainer->getZoneServer()->getPlayerManager();

	while (watchers.size() > 0) {
		ManagedReference<CreatureObject*> watcher = watchers.elementAt(0).getKey();

		Locker clocker(watcher, entertainer);

		playerManager->stopWatch(watcher, entertainer->getObjectID(), true, true, false);

		if (!watcher->isWatching())
			sendEntertainmentUpdate(watcher, 0, "", true);

		watchers.drop(watcher);
	}

	if (tickTask != nullptr && tickTask->isScheduled())
		tickTask->cancel();

	updateEntertainerMissionStatus(false, MissionTypes::DANCER);

	entertainer->notifyObservers(ObserverEventType::STOPENTERTAIN, entertainer);

	entertainer->dropObserver(ObserverEventType::POSTURECHANGED, observer);

	if (!dancing && !playingMusic) {
		ManagedReference<PlayerObject*> entPlayer = entertainer->getPlayerObject();
		if (entPlayer != nullptr && entPlayer->getPerformanceBuffTarget() != 0)
			entPlayer->setPerformanceBuffTarget(0);

		entertainer->dropActiveSession(SessionFacadeType::ENTERTAINING);
	}
}

bool EntertainingSessionImplementation::canHealBattleFatigue() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	if(entertainer->getSkillMod("private_med_battle_fatigue") > 0)
		return true;
	else
		return false;
}

bool EntertainingSessionImplementation::canGiveEntertainBuff() {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	if(entertainer->getSkillMod("private_buff_mind") > 0)
		return true;
	else
		return false;
}

// TODO: can this be simplified by doing the building check in the ticker?
void EntertainingSessionImplementation::addEntertainerFlourishBuff() {
	// Watchers that are in our group for passive buff
	VectorMap<ManagedReference<CreatureObject*>, EntertainingData>* patrons = nullptr;
	if (dancing) {
		patrons = &watchers;
	}
	else if (playingMusic) {
		patrons = &listeners;
	}
	if (patrons != nullptr) {
		for (int i = 0; i < patrons->size(); ++i) {
			ManagedReference<CreatureObject*> patron = patrons->elementAt(i).getKey();
			try {
				increaseEntertainerBuff(patron);
			} catch (Exception& e) {
				error("Unreported exception caught in EntertainingSessionImplementation::addEntertainerFlourishBuff()");
			}
		}
	} /*else
		System::out << "no patrons";*/

}

void EntertainingSessionImplementation::doFlourish(int flourishNumber, bool grantXp) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	int fid = flourishNumber;

	if (!dancing && !playingMusic) {
		entertainer->sendSystemMessage("@performance:flourish_not_performing");
		return;
	}

	PerformanceManager* performanceManager = SkillManager::instance()->getPerformanceManager();
	Performance* performance = nullptr;
	ManagedReference<Instrument*> instrument = getInstrument(entertainer);

	if (dancing)
		performance = performanceManager->getDance(performanceName);
	else if (playingMusic && instrument)
		performance = performanceManager->getSong(performanceName, instrument->getInstrumentType());
	else {
		cancelSession();
		return;
	}

	if (!performance) { // shouldn't happen
		StringBuffer msg;
		msg << "Performance was null.  Please report to www.swgemu.com/bugs ! Name: " << performanceName << " and Type: " << dec << instrument->getInstrumentType();

		entertainer->sendSystemMessage(msg.toString());
		return;
	}

	float baseActionDrain = performance->getActionPointsPerLoop() - (int)(entertainer->getHAM(CreatureAttribute::QUICKNESS)/35.f);

	//float baseActionDrain = -40 + (getQuickness() / 37.5);
	float flourishActionDrain = baseActionDrain / 15.0;

	int actionDrain = (int)round((flourishActionDrain * 10 + 0.5) / 20.0); // Round to nearest dec for actual int cost

	if (entertainer->getHAM(CreatureAttribute::ACTION) <= actionDrain) {
		entertainer->sendSystemMessage("@performance:flourish_too_tired");
	} else {
		entertainer->inflictDamage(entertainer, CreatureAttribute::ACTION, actionDrain, false, true);

		if (dancing) {
			StringBuffer msg;
			msg << "skill_action_" << fid;
			entertainer->doAnimation(msg.toString());
		} else if (playingMusic) {
			Flourish* flourish = new Flourish(entertainer, fid);
			entertainer->broadcastMessage(flourish, true);
		}

		//check to see how many flourishes have occurred this tick
		if(flourishCount < 5) {
			// Add buff
			addEntertainerFlourishBuff();

			// Grant Experience
			if(grantXp && flourishCount < 2)
				flourishXp += performance->getFlourishXpMod() / 2;

			flourishCount++;
		}
		entertainer->notifyObservers(ObserverEventType::FLOURISH, entertainer, fid);

		entertainer->sendSystemMessage("@performance:flourish_perform");
	}
}

void EntertainingSessionImplementation::addEntertainerBuffDuration(CreatureObject* creature, int performanceType, float duration) {
	int buffDuration = getEntertainerBuffDuration(creature, performanceType);

	buffDuration += duration;

	if (buffDuration > (210.0f + (10.0f / 60.0f)) ) // 3 hrs 10 seconds
		buffDuration = (210.0f + (10.0f / 60.0f)); // 3 hrs 10 seconds

	setEntertainerBuffDuration(creature, performanceType, buffDuration);
}

void EntertainingSessionImplementation::addEntertainerBuffStrength(CreatureObject* creature, int performanceType, float strength) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	int buffStrength = getEntertainerBuffStrength(creature, performanceType);


	float newBuffStrength = buffStrength + strength;

	float maxBuffStrength = 0.0f;	//cap based on enhancement skill
	if(dancing) {
		maxBuffStrength = (float) entertainer->getSkillMod("healing_dance_mind");
	}
	else if (playingMusic) {
		maxBuffStrength = (float) entertainer->getSkillMod("healing_music_mind");
	}

	if(maxBuffStrength > 125.0f)
		maxBuffStrength = 125.0f;	//cap at 125% power

	float factionPerkStrength = entertainer->getSkillMod("private_faction_buff_mind");

	ManagedReference<BuildingObject*> building = cast<BuildingObject*>(entertainer->getRootParent());

	if (building != nullptr && factionPerkStrength > 0 && building->isPlayerRegisteredWithin(entertainer->getObjectID())) {
		unsigned int buildingFaction = building->getFaction();
		unsigned int entFaction = entertainer->getFaction();

		if (entFaction != 0 && entFaction == buildingFaction && entertainer->getFactionStatus() == FactionStatus::OVERT) {
			maxBuffStrength += factionPerkStrength;
		}
	}

	//add xp based on % added to buff strength
	if (newBuffStrength  < maxBuffStrength) {
		healingXp += strength;
	}
	else {
		healingXp += maxBuffStrength - buffStrength;
		newBuffStrength = maxBuffStrength;
	}
	float cityPerkStrength = entertainer->getSkillMod("private_spec_entertainer");
	newBuffStrength += cityPerkStrength;
	//newBuffStrength = newBuffStrength;

	setEntertainerBuffStrength(creature, performanceType, newBuffStrength);
}

void EntertainingSessionImplementation::addWatcher(CreatureObject* creature) {
	if (watchers.contains(creature))
		watchers.drop(creature);

	EntertainingData data;

	watchers.put(creature, data);
}

void EntertainingSessionImplementation::addListener(CreatureObject* creature) {
	if (listeners.contains(creature))
		listeners.drop(creature);

	EntertainingData data;

	listeners.put(creature, data);
}

void EntertainingSessionImplementation::setEntertainerBuffDuration(CreatureObject* creature, int performanceType, float duration) {
	EntertainingData* data = nullptr;

	switch(performanceType) {
	case PerformanceType::DANCE:
		if (!watchers.contains(creature))
			return;

		data = &watchers.get(creature);

		break;
	case PerformanceType::MUSIC:
		if (!listeners.contains(creature))
			return;

		data = &listeners.get(creature);

		break;
	}

	if (data == nullptr)
		return;

	data->setDuration(duration);
}

int EntertainingSessionImplementation::getEntertainerBuffDuration(CreatureObject* creature, int performanceType) {
	EntertainingData* data = nullptr;

	switch(performanceType) {
	case PerformanceType::DANCE:
		if (!watchers.contains(creature))
			return 0;

		data = &watchers.get(creature);

		break;
	case PerformanceType::MUSIC:
		if (!listeners.contains(creature))
			return 0;

		data = &listeners.get(creature);

		break;
	}

	if (data == nullptr)
		return 0;

	return data->getDuration();
}

int EntertainingSessionImplementation::getEntertainerBuffStrength(CreatureObject* creature, int performanceType) {
	EntertainingData* data = nullptr;

	switch(performanceType) {
	case PerformanceType::DANCE:
		if (!watchers.contains(creature))
			return 0;

		data = &watchers.get(creature);

		break;
	case PerformanceType::MUSIC:
		if (!listeners.contains(creature))
			return 0;

		data = &listeners.get(creature);

		break;
	}

	if (data == nullptr)
		return 0;

	return data->getStrength();
}

int EntertainingSessionImplementation::getEntertainerBuffStartTime(CreatureObject* creature, int performanceType) {
	EntertainingData* data = nullptr;

	switch(performanceType) {
	case PerformanceType::DANCE:
		if (!watchers.contains(creature))
			return 0;

		data = &watchers.get(creature);

		break;
	case PerformanceType::MUSIC:
		if (!listeners.contains(creature))
			return 0;

		data = &listeners.get(creature);

		break;
	}

	if (data == nullptr)
		return 0;

	return data->getTimeStarted();
}

void EntertainingSessionImplementation::setEntertainerBuffStrength(CreatureObject* creature, int performanceType, float strength) {
	EntertainingData* data = nullptr;

	switch(performanceType) {
	case PerformanceType::DANCE:
		if (!watchers.contains(creature))
			return;

		data = &watchers.get(creature);

		break;
	case PerformanceType::MUSIC:
		if (!listeners.contains(creature))
			return;

		data = &listeners.get(creature);

		break;
	}

	if (data == nullptr)
		return;

	data->setStrength(strength);
}

void EntertainingSessionImplementation::sendEntertainmentUpdate(CreatureObject* creature, uint64 entid, const String& mood, bool updateEntValue) {
	CreatureObject* entertainer = this->entertainer.get();
		if (entertainer != nullptr) {
			if (entertainer->isPlayingMusic()) {
				creature->setListenToID(entid, true);
			}
			else if (entertainer->isDancing()) {
				creature->setWatchToID(entid);
			}
		}

	/*if (updateEntValue)
		creature->setTerrainNegotiation(0.8025000095f, true);*/

	String str = creature->getZoneServer()->getChatManager()->getMoodAnimation(mood);
	creature->setMoodString(str, true);
}

void EntertainingSessionImplementation::sendEntertainingUpdate(CreatureObject* creature, float entval, const String& performance, uint32 perfcntr, int instrid) {
	//creature->setTerrainNegotiation(entval, true);

	creature->setPerformanceAnimation(performance, false);
	creature->setPerformanceCounter(0, false);
	creature->setInstrumentID(instrid, false);

	CreatureObjectDeltaMessage6* dcreo6 = new CreatureObjectDeltaMessage6(creature);
	dcreo6->updatePerformanceAnimation(performance);
	dcreo6->updatePerformanceCounter(0);
	dcreo6->updateInstrumentID(instrid);
	dcreo6->close();

	creature->broadcastMessage(dcreo6, true);
}

void EntertainingSessionImplementation::activateEntertainerBuff(CreatureObject* creature, int performanceType) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	try {
		//Check if on Deny Service list
		if(isInDenyServiceList(creature)) {
			return;
		}

		ManagedReference<PlayerObject*> entPlayer = entertainer->getPlayerObject();
		//Check if the patron is a valid buff target
		//Whether it be passive(in the same group) or active (/setPerform target)
		if ((!entertainer->isGrouped() || entertainer->getGroupID() != creature->getGroupID())
				&& entPlayer->getPerformanceBuffTarget() != creature->getObjectID()) {
			return;
		}

		if(!canGiveEntertainBuff())
			return;

		// Returns the Number of Minutes for the Buff Duration
		float buffDuration = getEntertainerBuffDuration(creature, performanceType);

		if (buffDuration * 60 < 10.0f) { //10 sec minimum buff duration
			return;
		}

		//1 minute minimum listen/watch time
		int timeElapsed = time(0) - getEntertainerBuffStartTime(creature, performanceType);
		if(timeElapsed < 30) {
			creature->sendSystemMessage("You must listen or watch a performer for at least 30 seconds in order to gain the entertainer buffs.");
			return;
		}

		// Returns a % of base stat
		int campModTemp = 100;


		float buffStrength = getEntertainerBuffStrength(creature, performanceType) / 100.0f;

		if(buffStrength == 0)
			return;

		ManagedReference<PerformanceBuff*> oldBuff = nullptr;
		switch (performanceType){
		case PerformanceType::MUSIC:
		{
			uint32 focusBuffCRC = STRING_HASHCODE("performance_enhance_music_focus");
			uint32 willBuffCRC = STRING_HASHCODE("performance_enhance_music_willpower");
			oldBuff = cast<PerformanceBuff*>(creature->getBuff(focusBuffCRC));
			if (oldBuff != nullptr && oldBuff->getBuffStrength() > buffStrength)
				return;
			ManagedReference<PerformanceBuff*> focusBuff = new PerformanceBuff(creature, focusBuffCRC, buffStrength, buffDuration * 60, PerformanceBuffType::MUSIC_FOCUS);
			ManagedReference<PerformanceBuff*> willBuff = new PerformanceBuff(creature, willBuffCRC, buffStrength, buffDuration * 60, PerformanceBuffType::MUSIC_WILLPOWER);

			Locker locker(focusBuff);
			creature->addBuff(focusBuff);
			locker.release();

			Locker locker2(willBuff);
			creature->addBuff(willBuff);
			break;
		}
		case PerformanceType::DANCE:
		{
			uint32 mindBuffCRC = STRING_HASHCODE("performance_enhance_dance_mind");
			oldBuff = cast<PerformanceBuff*>(creature->getBuff(mindBuffCRC));
			if (oldBuff != nullptr && oldBuff->getBuffStrength() > buffStrength)
				return;
			ManagedReference<PerformanceBuff*> mindBuff = new PerformanceBuff(creature, mindBuffCRC, buffStrength, buffDuration * 60, PerformanceBuffType::DANCE_MIND);

			Locker locker(mindBuff);
			creature->addBuff(mindBuff);
			break;
		}
		}


	} catch(Exception& e) {

	}

}

void EntertainingSessionImplementation::updateEntertainerMissionStatus(bool entertaining, const int missionType) {
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	if (entertainer == nullptr) {
		return;
	}

	SceneObject* datapad = entertainer->getSlottedObject("datapad");

	if (datapad == nullptr) {
		return;
	}

	//Notify all missions of correct type.
	int datapadSize = datapad->getContainerObjectsSize();
	for (int i = 0; i < datapadSize; ++i) {
		if (datapad->getContainerObject(i)->isMissionObject()) {
			Reference<MissionObject*> datapadMission = datapad->getContainerObject(i).castTo<MissionObject*>();

			if (datapadMission != nullptr) {
				EntertainerMissionObjective* objective = cast<EntertainerMissionObjective*>(datapadMission->getMissionObjective());

				if (objective != nullptr && datapadMission->getTypeCRC() == MissionTypes::DANCER && missionType == MissionTypes::DANCER) {
					objective->setIsEntertaining(entertaining);
				} else if (objective != nullptr && datapadMission->getTypeCRC() == MissionTypes::MUSICIAN && missionType == MissionTypes::MUSICIAN) {
					objective->setIsEntertaining(entertaining);
				}
			}
		}
	}
}

void EntertainingSessionImplementation::increaseEntertainerBuff(CreatureObject* patron){
	ManagedReference<CreatureObject*> entertainer = this->entertainer.get();

	PerformanceManager* performanceManager = SkillManager::instance()->getPerformanceManager();
	Performance* performance = nullptr;

	ManagedReference<Instrument*> instrument = getInstrument(entertainer);

	if (performanceName == "")
		return;

	if (dancing) {
		performance = performanceManager->getDance(performanceName);
	} else if (playingMusic && instrument != nullptr) {
		performance = performanceManager->getSong(performanceName, instrument->getInstrumentType());
	} else {
		cancelSession();
		return;
	}

	if(!canGiveEntertainBuff())
		return;

	if (performance == nullptr) { // shouldn't happen
		return;
	}

	ManagedReference<PlayerObject*> entPlayer = entertainer->getPlayerObject();
	//Check if the patron is a valid buff target
	//Whether it be passive(in the same group) or active (/setPerform target)
	if ((!entertainer->isGrouped() || entertainer->getGroupID() != patron->getGroupID())
			&& entPlayer->getPerformanceBuffTarget() != patron->getObjectID()) {
		return;
	}

	if(isInDenyServiceList(patron))
		return;

	float buffAcceleration = 1 + ((float)entertainer->getSkillMod("accelerate_entertainer_buff") / 100.f);

	addEntertainerBuffDuration(patron, performance->getType(), 6.0f * buffAcceleration);
	addEntertainerBuffStrength(patron, performance->getType(), performance->getHealShockWound());

}

void EntertainingSessionImplementation::awardEntertainerExperience() {
	ManagedReference<CreatureObject*> player = this->entertainer.get();
	ManagedReference<PlayerManager*> playerManager = player->getZoneServer()->getPlayerManager();

	PerformanceManager* performanceManager = SkillManager::instance()->getPerformanceManager();
	Performance* performance = nullptr;
	ManagedReference<Instrument*> instrument = getInstrument(player);

	if (dancing)
		performance = performanceManager->getDance(performanceName);
	else if (playingMusic && instrument)
		performance = performanceManager->getSong(performanceName, instrument->getInstrumentType());

	if (player->isPlayerCreature() && performance != nullptr) {
		if (oldFlourishXp > flourishXp && (isDancing() || isPlayingMusic())) {
			flourishXp = oldFlourishXp;

			if (flourishXp > 0) {
				int flourishDec = (int)((float)performance->getFlourishXpMod() / 6.0f);
				flourishXp -= Math::max(1, flourishDec);
			}

			if (flourishXp < 0)
				flourishXp = 0;
		}

		if (flourishXp > 0 && (isDancing() || isPlayingMusic())) {
			String xptype;

			if (isDancing())
				xptype = "dance";
			else if (isPlayingMusic())
				xptype = "music";

			int groupBonusCount = 0;

			ManagedReference<GroupObject*> group = player->getGroup();

			if (group != nullptr) {
				for (int i = 0; i < group->getGroupSize(); ++i) {
					try {
						ManagedReference<CreatureObject *> groupMember = group->getGroupMember(i);

						if (groupMember != nullptr && groupMember->isPlayerCreature()) {
							Locker clocker(groupMember, player);

							if (groupMember != player && groupMember->isEntertaining() &&
								groupMember->isInRange(player, 40.0f) &&
								groupMember->hasSkill("social_entertainer_novice")) {
								++groupBonusCount;
							}
						}
					} catch (ArrayIndexOutOfBoundsException &exc) {
						warning("EntertainingSessionImplementation::awardEntertainerExperience " + exc.getMessage());
					}
				}
			}

			int xpAmount = flourishXp + performance->getBaseXp();

			int audienceSize = Math::min(getBandAudienceSize(), 50);
			float audienceMod = audienceSize / 50.f;
			float applauseMod = applauseCount / 100.f;

			float groupMod = groupBonusCount * 0.05;

			float totalBonus = 1.f + groupMod + audienceMod + applauseMod;

			xpAmount = ceil(xpAmount * totalBonus);

			// Apply 10x Entertainer XP multiplier
			xpAmount *= 10;

			if (playerManager != nullptr)
				playerManager->awardExperience(player, xptype, xpAmount, true);

			oldFlourishXp = flourishXp;
			flourishXp = 0;
		} else {
			oldFlourishXp = 0;
		}

		if (healingXp > 0) {
			String healxptype("entertainer_healing");

			// Apply 10x Entertainer XP multiplier
			healingXp *= 10;

			if (playerManager != nullptr)
				playerManager->awardExperience(player, healxptype, healingXp, true);

			healingXp = 0;
		}
	}

	applauseCount = 0;
	healingXp = 0;
	flourishCount = 0;
}

Vector<uint64> EntertainingSessionImplementation::getAudience() {
	Vector<uint64> audienceList;

	VectorMap<ManagedReference<CreatureObject*>, EntertainingData>* patrons = nullptr;
	if (dancing) {
		patrons = &watchers;
	} else if (playingMusic) {
		patrons = &listeners;
	}

	if (patrons == nullptr)
		return audienceList;

	for (int i = 0; i < patrons->size(); i++) {
		ManagedReference<CreatureObject*> patron = patrons->elementAt(i).getKey();

		if (patron != nullptr)
			audienceList.add(patron->getObjectID());
	}

	return audienceList;
}

int EntertainingSessionImplementation::getBandAudienceSize() {
	Vector<uint64> audienceList = getAudience();
	ManagedReference<CreatureObject *> player = entertainer.get();

	ManagedReference<GroupObject *> group = player->getGroup();

	if (group == nullptr)
		return audienceList.size();

	for (int i = 0; i < group->getGroupSize(); ++i) {
		try {
			ManagedReference<CreatureObject *> groupMember = group->getGroupMember(i);

			if (groupMember != nullptr && groupMember->isPlayerCreature()) {
				Locker clocker(groupMember, player);

				if (groupMember != player && groupMember->isEntertaining() &&
					groupMember->isInRange(player, 40.0f) &&
					groupMember->hasSkill("social_entertainer_novice")) {
					ManagedReference<EntertainingSession *> session = groupMember->getActiveSession(
							SessionFacadeType::ENTERTAINING).castTo<EntertainingSession *>();

					if (session == nullptr)
						continue;

					Vector<uint64> memberAudienceList = session->getAudience();

					for (int j = 0; j < memberAudienceList.size(); j++) {
						uint64 audienceID = memberAudienceList.get(j);

						if (!audienceList.contains(audienceID))
							audienceList.add(audienceID);
					}
				}
			}
		} catch (Exception &e) {
			warning("EntertainingSessionImplementation::getBandAudienceSize " + e.getMessage());
		}
	}

	return audienceList.size();
}
