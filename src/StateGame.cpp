﻿#include "StateGame.hpp"
#include "Collider.hpp"
#include "Collision.hpp"
#include "Game.hpp"
#include "GameProperties.hpp"
#include "HighScore.hpp"
#include "Hud.hpp"
#include "InputManager.hpp"
#include "MathHelper.hpp"
#include "SmartShape.hpp"
#include "SmartSprite.hpp"
#include "SmartTilemap.hpp"
#include "StateMenu.hpp"
#include "Timer.hpp"
#include "TweenAlpha.hpp"
#include "TweenPosition.hpp"
#include "TweenScale.hpp"

namespace {

void doHighscore(int const level_id, float const time)
{
    auto scores = loadHighscores();
    auto const currentScore = scores.at(level_id);
    if (time < currentScore || currentScore == 0) {
        scores.at(level_id) = time;
        saveHighscores(scores);
    }
}

} // namespace

StateGame::StateGame(int levelID, float timer)
    : m_levelID { levelID }
    , m_timer { timer }
    , m_startTimer { timer }
{
}

void StateGame::doCreate()
{
    float w = static_cast<float>(getGame()->getRenderTarget()->getSize().x);
    float h = static_cast<float>(getGame()->getRenderTarget()->getSize().y);
    m_hud = std::make_shared<Hud>();

    using JamTemplate::SmartShape;
    using JamTemplate::TweenAlpha;

    m_background = std::make_shared<SmartShape>();
    m_background->makeRect({ w, h });
    m_background->setColor(GP::PaletteBackground());
    m_background->setIgnoreCamMovement(true);
    m_background->update(0.0f);

    m_overlay = std::make_shared<SmartShape>();
    m_overlay->setIgnoreCamMovement(true);
    m_overlay->makeRect(sf::Vector2f { w, h });
    m_overlay->setColor(sf::Color { 0, 0, 0 });
    m_overlay->update(0);
    auto tw = TweenAlpha<SmartShape>::create(m_overlay, 0.5f, sf::Uint8 { 255 }, sf::Uint8 { 0 });
    tw->setSkipFrames();
    add(tw);

    m_textTimer = std::make_shared<JamTemplate::SmartText>();
    m_textTimer->loadFont("assets/font.ttf");
    m_textTimer->setCharacterSize(12U);
    m_textTimer->setText("Time: 0.00");
    m_textTimer->setPosition({ 8.0f, 6.0f });
    m_textTimer->setColor(GP::PaletteColor1());
    m_textTimer->update(0.0f);
    m_textTimer->SetTextAlign(JamTemplate::SmartText::TextAlign::LEFT);
    m_textTimer->setShadow(GP::PaletteFontShadow(), sf::Vector2f { 2, 2 });
    m_textTimer->setIgnoreCamMovement(true);

    m_tilemap = std::make_shared<JamTemplate::SmartTilemap>(
        std::filesystem::path(GP::getLevelList().at(m_levelID).first));
    m_tilemap->setScreenSizeHint(GP::ScreenSizeInGame(), getGame());
    m_tilemap->update(0.0f);
    doCreateInternal();
    add(m_hud);

    m_world = std::make_shared<b2World>(b2Vec2 { 0, GP::GravityStrength() });

    m_colliders = std::make_shared<JamTemplate::ObjectGroup<Collider>>();
    auto const lol = m_tilemap->getObjectGroups().at(GP::ColliderLayerName());
    for (auto& rect : lol) {
        auto coll = std::make_shared<Collider>(m_world, rect);
        add(coll);
        m_colliders->push_back(coll);
    }
    add(m_colliders);

    // get start position
    sf::Vector2f startPosition;
    auto const other = m_tilemap->getObjectGroups().at(GP::OtherLayerName());
    for (auto const r : other) {
        if (r.m_type == "start") {
            startPosition = r.position;
            // std::cout << startPosition.x << " " << startPosition.y << "\n";
        } else if (r.m_type == "end") {
            m_endZone = std::make_shared<JamTemplate::SmartShape>();
            m_endZone->makeRect(r.sizeDiagonal);
            m_endZone->setColor(sf::Color { 255, 255, 255, 100 });
            m_endZone->setPosition(r.position);
            m_endZone->setRotation(r.rotation);
        } else if (r.m_type == "damaging") {
            auto z = std::make_shared<JamTemplate::SmartShape>();
            z->makeRect(r.sizeDiagonal);
            z->setColor(sf::Color { 255, 64, 0, 0 });
            z->setPosition(r.position);
            z->setRotation(r.rotation);
            z->update(0.0f);
            m_damagingZones.push_back(z);
        } else if (r.m_type == "blocking") {
            auto z = std::make_shared<JamTemplate::SmartShape>();
            z->makeRect(r.sizeDiagonal);
            z->setColor(sf::Color { 68, 112, 45, 20 });
            z->setPosition(r.position);
            z->setRotation(r.rotation);
            z->update(0.0f);
            m_blockingZones.push_back(z);
        }
    }
    getGame()->setCamOffset(startPosition - GP::ScreenSizeInGame() / 2.0f);

    b2BodyDef bodyDef;
    bodyDef.type = b2_dynamicBody;
    bodyDef.position = JamTemplate::C::vec(startPosition);
    m_target = std::make_shared<Target>(m_world, &bodyDef);

    m_contactListener
        = std::make_shared<TargetContactListener>([this](float d) { handleDamage(d); });
    m_world->SetContactListener(m_contactListener.get());

    add(m_target);
    m_lastCollisionAge = getAge();

    m_vignette = std::make_shared<JamTemplate::SmartSprite>();

    m_vignette->loadSprite("#v#" + std::to_string(static_cast<int>(GP::ScreenSizeInGame().x)) + "#"
        + std::to_string(static_cast<int>(GP::ScreenSizeInGame().y)));
    m_vignette->setIgnoreCamMovement(true);
    m_vignette->update(0.0f);

    m_particlesDust = std::make_shared<JamTemplate::ParticleSystem<JamTemplate::SmartSprite, 100>>(
        []() {
            auto const s = std::make_shared<JamTemplate::SmartSprite>();
            s->loadSprite("#g#16#255");
            s->setColor(GP::PaletteColorGlow());
            s->setPosition({ -500, -500 });
            return s;
        },
        [this](std::shared_ptr<JamTemplate::SmartSprite> s) {
            s->setPosition(m_target->getPosition() - sf::Vector2f { 8, 8 });

            auto twa = JamTemplate::TweenAlpha<JamTemplate::SmartSprite>::create(s, 0.5f, 100, 0);
            add(twa);

            auto tws = JamTemplate::TweenScale<JamTemplate::SmartSprite>::create(
                s, 0.75f, sf::Vector2f { 1.0f, 1.0f }, sf::Vector2f { 0.0f, 0.0f });
            add(tws);
        });
    add(m_particlesDust);
    auto t = std::make_shared<JamTemplate::Timer>(0.025f, [this]() { m_particlesDust->Fire(1); });
    add(t);

    m_particlesBreak = std::make_shared<JamTemplate::ParticleSystem<JamTemplate::SmartShape, 100>>(
        []() {
            auto const s = std::make_shared<JamTemplate::SmartShape>();
            s->setPosition({ -500, -500 });
            s->makeRect({ 2, 2 });
            s->setColor(sf::Color { 246, 118, 5 });
            return s;
        },
        [this](std::shared_ptr<JamTemplate::SmartShape> s) {
            s->setPosition(m_target->getPosition());

            auto const start = m_target->getPosition();
            auto const end = start
                + JamTemplate::Random::getRandomPointin(sf::FloatRect { -100, -100, 200, 200 });
            auto twp
                = JamTemplate::TweenPosition<JamTemplate::SmartShape>::create(s, 1.0f, start, end);
            add(twp);

            auto twa = JamTemplate::TweenAlpha<JamTemplate::SmartShape>::create(s, 0.125f, 255, 0);
            twa->setStartDelay(0.2f);
            twa->setSkipFrames(2);
            add(twa);
        });
    add(m_particlesBreak);

    m_particlesBeam = std::make_shared<JamTemplate::ParticleSystem<JamTemplate::SmartShape, 100>>(
        []() {
            auto const s = std::make_shared<JamTemplate::SmartShape>();
            s->makeRect({ 1, 4 });
            s->setColor(sf::Color { 213, 214, 219, 120 });
            return s;
        },
        [this](std::shared_ptr<JamTemplate::SmartShape> s) {
            auto const start
                = sf::Vector2f { m_target->getBeamPosX(), m_target->getTargetPosition().y }
                + sf::Vector2f { JamTemplate::Random::getFloatGauss(0, 8),
                      JamTemplate::Random::getFloat(-30, 50) };

            s->setPosition(start);

            auto const end
                = start + sf::Vector2f { 0, -100 + JamTemplate::Random::getFloatGauss(0, 5) };

            auto twp
                = JamTemplate::TweenPosition<JamTemplate::SmartShape>::create(s, 1.0f, start, end);
            add(twp);

            auto twa = JamTemplate::TweenAlpha<JamTemplate::SmartShape>::create(s, 0.25f, 120, 0);
            twa->setStartDelay(0.6f);
            twa->setSkipFrames(2);
            add(twa);

            auto tws = JamTemplate::TweenScale<JamTemplate::SmartShape>::create(
                s, 1.0f, sf::Vector2f { 1.0f, 1.0f }, sf::Vector2f { 1.0f, 2.0f });
            add(tws);
        });
    add(m_particlesBeam);
    m_lastBeamParticle = 0;

    ////////////////////////////// Sounds
    m_sndBufAlarm1.loadFromFile("assets/sounds/alarm_1.ogg");
    m_sndBufAlarm2.loadFromFile("assets/sounds/alarm_2.ogg");
    m_sndBufAlarm3.loadFromFile("assets/sounds/alarm_3.ogg");
    m_sndBufCollision.loadFromFile("assets/sounds/soft_collision_1.ogg");
    m_sndAlarm1.setBuffer(m_sndBufAlarm1);
    m_sndAlarm2.setBuffer(m_sndBufAlarm2);
    m_sndAlarm3.setBuffer(m_sndBufAlarm3);
    m_sndCollision.setBuffer(m_sndBufCollision);

    m_sndAlarm1.setVolume(25);
    m_sndAlarm2.setVolume(25);
    m_sndAlarm3.setVolume(25);

    // m_sndAlarm1.setLoop(true);
    /*m_sndAlarm2.setLoop(true);
    m_sndAlarm3.setLoop(true);*/

    if (m_restartMusic) {
        getGame()->m_music.openFromFile("assets/sounds/song_ingame_1.ogg");
        getGame()->m_music.play();
        m_restartMusic = false;
    }
}

bool StateGame::m_restartMusic { true };

void StateGame::doCreateInternal() { }

void StateGame::doInternalUpdate(float const elapsed)
{
    if (!m_isDead) {
        m_timer += elapsed;
        m_lastBeamParticle -= elapsed;

        if (JamTemplate::InputManager::justReleased(GP::KeyToggleDrawObjectGroups())) {
            m_tilemap->toggleObjectGroupVisibility();
        }
        if (JamTemplate::InputManager::justPressed(sf::Keyboard::R)) {
            restartLevel();
        }

        int32 velocityIterations = 6;
        int32 positionIterations = 2;
        m_world->Step(elapsed, velocityIterations, positionIterations);

        if (JamTemplate::InputManager::justPressed(sf::Keyboard::Escape)) {
            getGame()->switchState(std::make_shared<StateMenu>());
        }

        if (JamTemplate::InputManager::pressed(sf::Mouse::Left)) {
            if (m_lastBeamParticle <= 0) {
                m_particlesBeam->Fire(1);
                m_lastBeamParticle = 0.12f;
            }
        }

        doScrolling(elapsed);
        if (JamTemplate::Collision::BoundingBoxTest(m_endZone, m_target->getTarget())) {
            unsigned int nextLevelID = m_levelID + 1U;

            doHighscore(m_levelID, m_timer);

            if (nextLevelID != GP::getLevelList().size()) {
                getGame()->switchState(std::make_shared<StateGame>(nextLevelID, 0.0f));
            } else {
                getGame()->switchState(std::make_shared<StateMenu>());
            }
        }
        m_target->setVerticalBeam(!playerIsInBlockingZone());

    } else {
        handleDeath(elapsed);
    }

    m_background->update(elapsed);
    m_overlay->update(elapsed);
    m_vignette->update(elapsed);
    m_tilemap->update(elapsed);

    std::string const str = JamTemplate::MathHelper::floatToStringWithXDigits(m_timer, 2);
    m_textTimer->setText("Time: " + str);
    m_textTimer->update(elapsed);

    if (m_endZone) {
        m_endZone->update(elapsed);
    }
    for (auto damagingZone : m_damagingZones) {
        damagingZone->update(elapsed);
    }
    for (auto z : m_blockingZones) {
        z->update(elapsed);
    }
}

void StateGame::doScrolling(float const elapsed)
{
    if (m_isDead) {
        return;
    }
    auto const mps = JamTemplate::InputManager::getMousePositionScreen();
    auto const tpw = m_target->getTargetPosition();
    auto const tps = getGame()->getRenderWindow()->mapCoordsToPixel(tpw, *getGame()->getView())
        / static_cast<int>(GP::Zoom());

    auto const mpsc
        = sf::Vector2f { JamTemplate::MathHelper::clamp(mps.x, 0.0f, GP::ScreenSizeInGame().x),
              JamTemplate::MathHelper::clamp(mps.y, 0.0f, GP::ScreenSizeInGame().y) };

    if (mpsc.x < GP::ScrollBoundary()) {
        getGame()->moveCam(sf::Vector2f { -GP::ScrollSpeedX(), 0 } * elapsed);
    }
    if (mpsc.x > GP::ScreenSizeInGame().x - GP::ScrollBoundary()) {
        getGame()->moveCam(sf::Vector2f { GP::ScrollSpeedX(), 0 } * elapsed);
    }

    if (tps.y < GP::ScrollBoundary()) {
        float const f = (tps.y < 0) ? 2.0f : 1.0f;
        getGame()->moveCam(sf::Vector2f { 0, -GP::ScrollSpeedY() * f } * elapsed);
    }
    if (tps.y > GP::ScreenSizeInGame().y - GP::ScrollBoundary()) {
        float const f = (tps.y > GP::ScreenSizeInGame().y) ? 2.0f : 1.0f;

        getGame()->moveCam(sf::Vector2f { 0, GP::ScrollSpeedY() } * f * elapsed);
    }

    // limit cam movement to map
    auto const cpx = getGame()->getCamOffset().x;
    auto const cpy = getGame()->getCamOffset().y;

    auto const cw = GP::ScreenSizeInGame().x;
    auto const ch = GP::ScreenSizeInGame().y;

    auto const msxi = m_tilemap->getMapSizeInTiles().x;
    auto const msyi = m_tilemap->getMapSizeInTiles().y;
    auto const msx = static_cast<float>(msxi * GP::TileSizeInPixel());
    auto const msy = static_cast<float>(msyi * GP::TileSizeInPixel());

    auto cpxc = JamTemplate::MathHelper::clamp(cpx, 0.0f, msx - cw);
    auto cpyc = JamTemplate::MathHelper::clamp(cpy, -10.0f * GP::TileSizeInPixel(), msy - ch);

    // std::cout << cpx << " " << msx << " " << cpxc << "\n";

    getGame()->setCamOffset(sf::Vector2f { cpxc, cpyc });
}

void StateGame::doInternalDraw() const
{
    m_background->draw(getGame()->getRenderTarget());
    m_tilemap->draw(getGame()->getRenderTarget());
    drawObjects();

    for (auto damagingZone : m_damagingZones) {
        damagingZone->draw(getGame()->getRenderTarget());
    }
    for (auto z : m_blockingZones) {
        z->draw(getGame()->getRenderTarget());
    }

    m_vignette->setColor(sf::Color::Black);
    m_vignette->draw(getGame()->getRenderTarget());

    for (auto i = 0U; i != m_target->getDamage(); ++i) {
        m_vignette->setColor(sf::Color { 0, 0, 0, 175 });
        m_vignette->draw(getGame()->getRenderTarget());
    }
    m_overlay->draw(getGame()->getRenderTarget());

    m_textTimer->draw(getGame()->getRenderTarget());
}

void StateGame::handleDamage(float damage)
{
    if (m_isDead) {
        return;
    }
    if (getAge() < m_lastCollisionAge + GP::InvulnerabilityAge()) {
        return;
    }

    bool damagingZoneOverride = false;
    for (auto damZone : m_damagingZones) {
        if (JamTemplate::Collision::BoundingBoxTest(damZone, m_target->getTarget())) {
            damagingZoneOverride = true;
            break;
        }
    }

    if (damage > GP::AllowedCollisionSpeed() || damagingZoneOverride) {
        getGame()->shake(GP::StrongShakeDuration(), GP::StrongShakeIntensity());
        m_overlay->flash(GP::StrongFlashDuration(), GP::StrongFlashColor());
        unsigned int newDamage = m_target->getDamage() + 1;
        m_particlesBreak->Fire(20);
        m_sndCollision.play();
        if (newDamage > GP::MaxCrystalDamage()) {
            m_isDead = true;

            return;
        }
        m_target->setDamage(newDamage);

        if (m_target->getDamage() == 2) {
            m_sndAlarm2.stop();
            m_sndAlarm3.stop();
            m_sndAlarm1.play();
        } else if (m_target->getDamage() == 3) {
            m_sndAlarm1.stop();
            m_sndAlarm3.stop();
            m_sndAlarm2.play();
        } else if (m_target->getDamage() == 4) {
            m_sndAlarm1.stop();
            m_sndAlarm2.stop();
            m_sndAlarm3.play();
        }
    } else if (damage > GP::AllowedCollisionSpeed() / 4.0f) {
        m_particlesBreak->Fire(10);
        m_sndCollision.play();
    }
    m_lastCollisionAge = getAge();
}

void StateGame::handleDeath(float const elapsed)
{
    m_target->kill();

    // Allow for skipping the animation
    for (auto& k : JamTemplate::InputHelper::getAllKeys()) {
        if (JamTemplate::InputManager::justReleased(k)) {
            restartLevel();
        }
    }

    // scroll up + tween to black + fade out sounds
    getGame()->moveCam(sf::Vector2f { 0, -GP::ScrollSpeedY() * elapsed });
    m_target->m_sndTraktorLoop.setVolume(m_target->m_sndTraktorLoop.getVolume() * 0.95f);
    m_sndAlarm1.setVolume(m_sndAlarm1.getVolume() * 0.95f);
    m_sndAlarm2.setVolume(m_sndAlarm2.getVolume() * 0.95f);
    m_sndAlarm3.setVolume(m_sndAlarm3.getVolume() * 0.95f);

    if (!m_alreadyTweening) {

        m_particlesDust->kill();
        auto tw = JamTemplate::TweenAlpha<JamTemplate::SmartShape>::create(
            m_overlay, 1.75f, sf::Uint8 { 0 }, sf::Uint8 { 255 });
        tw->addCompleteCallback([this]() { restartLevel(); });
        tw->setSkipFrames();
        tw->setAgePercentConversion([](float const in) { return std::pow(in, 0.25f); });
        add(tw);
    }
    m_alreadyTweening = true;
}

bool StateGame::playerIsInBlockingZone()
{
    for (auto z : m_blockingZones) {
        if (JamTemplate::Collision::BoundingBoxTest(z, m_target->getTarget())) {
            // std::cout << "overlap\n";
            return true;
        }
    }
    // std::cout << "NO overlap\n";
    return false;
}

void StateGame::restartLevel()
{
    getGame()->switchState(std::make_shared<StateGame>(m_levelID, m_startTimer));
}
