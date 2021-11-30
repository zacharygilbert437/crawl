/**
 * @file
 * @brief Monster information that may be passed to the user.
 *
 * Used to fill the monster pane and to pass monster info to Lua.
**/

#include "AppHdr.h"

#include "mon-info.h"

#include <algorithm>
#include <sstream>

#include "act-iter.h"
#include "artefact.h"
#include "attack.h"
#include "colour.h"
#include "coordit.h"
#include "english.h"
#include "env.h"
#include "ghost.h"
#include "item-prop.h"
#include "item-status-flag-type.h"
#include "libutil.h"
#include "los.h"
#include "message.h"
#include "mon-behv.h"
#include "mon-book.h"
#include "mon-death.h" // ELVEN_IS_ENERGIZED_KEY
#include "mon-info-flag-name.h"
#include "mon-tentacle.h"
#include "nearby-danger.h"
#include "options.h"
#include "religion.h"
#include "skills.h"
#include "spl-goditem.h" // dispellable_enchantments
#include "state.h"
#include "stringutil.h"
#include "tag-version.h"
#ifdef USE_TILE
#include "tilepick.h"
#endif
#include "traps.h"

#define SPELL_HD_KEY "spell_hd"
#define NIGHTVISION_KEY "nightvision"

/// Simple 1:1 mappings between monster enchantments & info flags.
static map<enchant_type, monster_info_flags> trivial_ench_mb_mappings = {
    { ENCH_BERSERK,         MB_BERSERK },
    { ENCH_CORONA,          MB_GLOWING },
    { ENCH_SILVER_CORONA,   MB_GLOWING },
    { ENCH_SLOW,            MB_SLOWED },
    { ENCH_SICK,            MB_SICK },
    { ENCH_INSANE,          MB_INSANE },
    { ENCH_HASTE,           MB_HASTED },
    { ENCH_MIGHT,           MB_STRONG },
    { ENCH_CONFUSION,       MB_CONFUSED },
    { ENCH_INVIS,           MB_INVISIBLE },
    { ENCH_CHARM,           MB_CHARMED },
    { ENCH_STICKY_FLAME,    MB_BURNING },
    { ENCH_PETRIFIED,       MB_PETRIFIED },
    { ENCH_PETRIFYING,      MB_PETRIFYING },
    { ENCH_LOWERED_WL,      MB_LOWERED_WL },
    { ENCH_SWIFT,           MB_SWIFT },
    { ENCH_SILENCE,         MB_SILENCING },
    { ENCH_PARALYSIS,       MB_PARALYSED },
    { ENCH_SOUL_RIPE,       MB_POSSESSABLE },
    { ENCH_REGENERATION,    MB_REGENERATION },
    { ENCH_STRONG_WILLED,   MB_STRONG_WILLED },
    { ENCH_MIRROR_DAMAGE,   MB_MIRROR_DAMAGE },
    { ENCH_DAZED,           MB_DAZED },
    { ENCH_MUTE,            MB_MUTE },
    { ENCH_BLIND,           MB_BLIND },
    { ENCH_DUMB,            MB_DUMB },
    { ENCH_MAD,             MB_MAD },
    { ENCH_INNER_FLAME,     MB_INNER_FLAME },
    { ENCH_BREATH_WEAPON,   MB_BREATH_WEAPON },
    { ENCH_ROLLING,         MB_ROLLING },
    { ENCH_WRETCHED,        MB_WRETCHED },
    { ENCH_SCREAMED,        MB_SCREAMED },
    { ENCH_WORD_OF_RECALL,  MB_WORD_OF_RECALL },
    { ENCH_INJURY_BOND,     MB_INJURY_BOND },
    { ENCH_FLAYED,          MB_FLAYED },
    { ENCH_WEAK,            MB_WEAK },
    { ENCH_DIMENSION_ANCHOR, MB_DIMENSION_ANCHOR },
    { ENCH_TOXIC_RADIANCE,  MB_TOXIC_RADIANCE },
    { ENCH_GRASPING_ROOTS,  MB_GRASPING_ROOTS },
    { ENCH_FIRE_VULN,       MB_FIRE_VULN },
    { ENCH_POLAR_VORTEX,         MB_VORTEX },
    { ENCH_POLAR_VORTEX_COOLDOWN, MB_VORTEX_COOLDOWN },
    { ENCH_BARBS,           MB_BARBS },
    { ENCH_POISON_VULN,     MB_POISON_VULN },
    { ENCH_AGILE,           MB_AGILE },
    { ENCH_FROZEN,          MB_FROZEN },
    { ENCH_BLACK_MARK,      MB_BLACK_MARK },
    { ENCH_SAP_MAGIC,       MB_SAP_MAGIC },
    { ENCH_CORROSION,       MB_CORROSION },
    { ENCH_REPEL_MISSILES,  MB_REPEL_MSL },
    { ENCH_RESISTANCE,      MB_RESISTANCE },
    { ENCH_HEXED,           MB_HEXED },
    { ENCH_BRILLIANCE_AURA, MB_BRILLIANCE_AURA },
    { ENCH_EMPOWERED_SPELLS, MB_EMPOWERED_SPELLS },
    { ENCH_GOZAG_INCITE,    MB_GOZAG_INCITED },
    { ENCH_PAIN_BOND,       MB_PAIN_BOND },
    { ENCH_IDEALISED,       MB_IDEALISED },
    { ENCH_BOUND_SOUL,      MB_BOUND_SOUL },
    { ENCH_INFESTATION,     MB_INFESTATION },
    { ENCH_STILL_WINDS,     MB_STILL_WINDS },
    { ENCH_VILE_CLUTCH,     MB_VILE_CLUTCH },
    { ENCH_WATERLOGGED,     MB_WATERLOGGED },
    { ENCH_RING_OF_THUNDER, MB_CLOUD_RING_THUNDER },
    { ENCH_RING_OF_FLAMES,  MB_CLOUD_RING_FLAMES },
    { ENCH_RING_OF_CHAOS,   MB_CLOUD_RING_CHAOS },
    { ENCH_RING_OF_MUTATION,MB_CLOUD_RING_MUTATION },
    { ENCH_RING_OF_FOG,     MB_CLOUD_RING_FOG },
    { ENCH_RING_OF_ICE,     MB_CLOUD_RING_ICE },
    { ENCH_RING_OF_DRAINING,MB_CLOUD_RING_DRAINING },
    { ENCH_RING_OF_ACID,    MB_CLOUD_RING_ACID },
    { ENCH_CONCENTRATE_VENOM, MB_CONCENTRATE_VENOM },
    { ENCH_FIRE_CHAMPION,   MB_FIRE_CHAMPION },
};

static monster_info_flags ench_to_mb(const monster& mons, enchant_type ench)
{
    // Suppress silly-looking combinations, even if they're
    // internally valid.
    if (mons.paralysed() && (ench == ENCH_SLOW || ench == ENCH_HASTE
                      || ench == ENCH_SWIFT
                      || ench == ENCH_PETRIFIED
                      || ench == ENCH_PETRIFYING))
    {
        return NUM_MB_FLAGS;
    }

    if (ench == ENCH_PETRIFIED && mons.has_ench(ENCH_PETRIFYING))
        return NUM_MB_FLAGS;

    // Don't claim that naturally 'confused' monsters are especially bewildered
    if (ench == ENCH_CONFUSION && mons_class_flag(mons.type, M_CONFUSED))
        return NUM_MB_FLAGS;

    const monster_info_flags *flag = map_find(trivial_ench_mb_mappings, ench);
    if (flag)
        return *flag;

    switch (ench)
    {
    case ENCH_HELD:
        return get_trapping_net(mons.pos(), true) == NON_ITEM
               ? MB_WEBBED : MB_CAUGHT;
    case ENCH_WATER_HOLD:
        if (mons.res_water_drowning() > 0)
            return MB_WATER_HOLD;
        else
            return MB_WATER_HOLD_DROWN;
    case ENCH_DRAINED:
        {
            const bool heavily_drained = mons.get_ench(ench).degree
                                         >= mons.get_experience_level() / 2;
            return heavily_drained ? MB_HEAVILY_DRAINED : MB_LIGHTLY_DRAINED;
        }
    case ENCH_SPELL_CHARGED:
        if (mons.get_ench(ench).degree < max_mons_charge(mons.type))
            return MB_PARTIALLY_CHARGED;
        return MB_FULLY_CHARGED;
    case ENCH_POISON:
        if (mons.get_ench(ench).degree == 1)
            return MB_POISONED;
        else if (mons.get_ench(ench).degree < MAX_ENCH_DEGREE_DEFAULT)
            return MB_MORE_POISONED;
        else
            return MB_MAX_POISONED;
    case ENCH_SHORT_LIVED:
    case ENCH_SLOWLY_DYING:
        if (mons.type == MONS_WITHERED_PLANT)
            return MB_CRUMBLING;
        if (mons_class_is_fragile(mons.type))
            return MB_WITHERING;
        return MB_SLOWLY_DYING;
    default:
        return NUM_MB_FLAGS;
    }
}

static bool _blocked_ray(const coord_def &where,
                         dungeon_feature_type* feat = nullptr)
{
    if (exists_ray(you.pos(), where, opc_solid_see)
        || !exists_ray(you.pos(), where, opc_default))
    {
        return false;
    }
    if (feat == nullptr)
        return true;
    *feat = ray_blocker(you.pos(), where);
    return true;
}

static bool _is_public_key(string key)
{
    if (key == HELPLESS_KEY
     || key == "feat_type"
     || key == "glyph"
     || key == DBNAME_KEY
     || key == MONSTER_TILE_KEY
#ifdef USE_TILE
     || key == TILE_NUM_KEY
#endif
     || key == "tile_idx"
     || key == CUSTOM_SPELLS_KEY
     || key == ELVEN_IS_ENERGIZED_KEY
     || key == MUTANT_BEAST_FACETS
     || key == MUTANT_BEAST_TIER
     || key == DOOM_HOUND_HOWLED_KEY
     || key == MON_GENDER_KEY
     || key == SEEN_SPELLS_KEY
     || key == KNOWN_MAX_HP_KEY
     || key == VAULT_HD_KEY
     || key == POLY_SET_KEY)
    {
        return true;
    }

    return false;
}

static int quantise(int value, int stepsize)
{
    return value + stepsize - value % stepsize;
}

// Returns true if using a directional tentacle tile would leak
// information the player doesn't have about a tentacle segment's
// current position.
static bool _tentacle_pos_unknown(const monster *tentacle,
                                  const coord_def orig_pos)
{
    // We can see the segment, no guessing necessary.
    if (!tentacle->submerged())
        return false;

    const coord_def t_pos = tentacle->pos();

    // Checks whether there are any positions adjacent to the
    // original tentacle that might also contain the segment.
    for (adjacent_iterator ai(orig_pos); ai; ++ai)
    {
        if (*ai == t_pos)
            continue;

        if (!in_bounds(*ai))
            continue;

        if (you.pos() == *ai)
            continue;

        // If there's an adjacent deep water tile, the segment
        // might be there instead.
        if (env.grid(*ai) == DNGN_DEEP_WATER)
        {
            const monster *mon = monster_at(*ai);
            if (mon && you.can_see(*mon))
            {
                // Could originate from the kraken.
                if (mon->type == MONS_KRAKEN)
                    return true;

                // Otherwise, we know the segment can't be there.
                continue;
            }
            return true;
        }

        if (env.grid(*ai) == DNGN_SHALLOW_WATER)
        {
            const monster *mon = monster_at(*ai);

            // We know there's no segment there.
            if (!mon)
                continue;

            // Disturbance in shallow water -> might be a tentacle.
            if (mon->type == MONS_KRAKEN || mon->submerged())
                return true;
        }
    }

    // Using a directional tile leaks no information.
    return false;
}

static void _translate_tentacle_ref(monster_info& mi, const monster* m,
                                    const string &key)
{
    if (!m->props.exists(key))
        return;

    const monster* other = monster_by_mid(m->props[key].get_int());
    if (other)
    {
        coord_def h_pos = other->pos();
        // If the tentacle and the other segment are no longer adjacent
        // (distortion etc.), just treat them as not connected.
        if (adjacent(m->pos(), h_pos)
            && !mons_is_zombified(*other)
            && !_tentacle_pos_unknown(other, m->pos()))
        {
            mi.props[key] = h_pos - m->pos();
        }
    }
}

/// is the given monster_info a hydra, zombie hydra, lerny, etc?
static bool _has_hydra_multi_attack(const monster_info &mi)
{
    return mons_genus(mi.type) == MONS_HYDRA
           || mons_genus(mi.base_type) == MONS_HYDRA
           || mons_species(mi.base_type) == MONS_SERPENT_OF_HELL;
}

monster_info::monster_info(monster_type p_type, monster_type p_base_type)
{
    mb.reset();
    attitude = ATT_HOSTILE;
    pos = coord_def(0, 0);

    type = p_type;

    // give 'job' monsters a default race.
    const bool classy_drac = mons_is_draconian_job(type) || type == MONS_TIAMAT;
    base_type = p_base_type != MONS_NO_MONSTER ? p_base_type
                : classy_drac ? MONS_DRACONIAN
                : type;

    if (_has_hydra_multi_attack(*this))
        num_heads = 1;
    else
        number = 0;

    _colour = COLOUR_INHERIT;

    holi = mons_class_holiness(type);

    mintel = mons_class_intel(type);

    hd = mons_class_hit_dice(type);
    ac = get_mons_class_ac(type);
    ev = base_ev = get_mons_class_ev(type);
    mresists = get_mons_class_resists(type);
    mr = mons_class_willpower(type, base_type);
    can_see_invis = mons_class_sees_invis(type, base_type);

    mitemuse = mons_class_itemuse(type);

    mbase_speed = mons_class_base_speed(type);
    menergy = mons_class_energy(type);

    if (mons_class_flag(type, M_FLIES) || mons_class_flag(base_type, M_FLIES))
        mb.set(MB_AIRBORNE);

    if (mons_class_wields_two_weapons(type)
        || mons_class_wields_two_weapons(base_type))
    {
        mb.set(MB_TWO_WEAPONS);
    }

    if (!mons_class_can_regenerate(type)
        || !mons_class_can_regenerate(base_type))
    {
        mb.set(MB_NO_REGEN);
    }

    threat = MTHRT_UNDEF;

    dam = MDAM_OKAY;

    fire_blocker = DNGN_UNSEEN;

    if (mons_is_pghost(type))
    {
        i_ghost.species = SP_HUMAN;
        i_ghost.job = JOB_WANDERER;
        i_ghost.religion = GOD_NO_GOD;
        i_ghost.best_skill = SK_FIGHTING;
        i_ghost.best_skill_rank = 2;
        i_ghost.xl_rank = 3;
        hd = ghost_rank_to_level(i_ghost.xl_rank);
        i_ghost.ac = 5;
        i_ghost.damage = 5;
    }

    if (mons_is_draconian_job(type))
    {
        ac += get_mons_class_ac(base_type);
        ev += get_mons_class_ev(base_type);
    }

    if (mons_is_unique(type))
    {
        if (type == MONS_LERNAEAN_HYDRA
            || type == MONS_ROYAL_JELLY
            || mons_species(type) == MONS_SERPENT_OF_HELL)
        {
            mb.set(MB_NAME_THE);
        }
        else
        {
            mb.set(MB_NAME_UNQUALIFIED);
            mb.set(MB_NAME_THE);
        }
    }

    for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
        attack[i] = get_monster_data(type)->attack[i];

    props.clear();
    // Change this in sync with monster::cloud_immune()
    if (type == MONS_CLOUD_MAGE)
        props[CLOUD_IMMUNE_MB_KEY] = true;

    // At least enough to keep from crashing. TODO: allow specifying these?
    if (type == MONS_MUTANT_BEAST)
    {
        props[MUTANT_BEAST_TIER].get_short() = BT_FIRST;
        for (int i = BF_FIRST; i < NUM_BEAST_FACETS; ++i)
            props[MUTANT_BEAST_FACETS].get_vector().push_back(i);
    }

    client_id = 0;
}

static description_level_type _article_for(const actor* a)
{
    // Player gets DESC_A, but that doesn't really matter.
    const monster * const m = a->as_monster();
    return m && m->friendly() ? DESC_YOUR : DESC_A;
}

monster_info::monster_info(const monster* m, int milev)
{
    ASSERT(m); // TODO: change to const monster &mon
    mb.reset();
    attitude = ATT_HOSTILE;
    pos = m->pos();

    attitude = mons_attitude(*m);

    type = m->type;
    threat = milev <= MILEV_NAME ? MTHRT_TRIVIAL : mons_threat_level(*m);

    props.clear();
    // CrawlHashTable::begin() const can fail if the hash is empty.
    if (!m->props.empty())
    {
        for (const auto &entry : m->props)
            if (_is_public_key(entry.first))
                props[entry.first] = entry.second;
    }

    // Translate references to tentacles into just their locations
    if (mons_is_tentacle_or_tentacle_segment(type))
    {
        _translate_tentacle_ref(*this, m, INWARDS_KEY);
        _translate_tentacle_ref(*this, m, OUTWARDS_KEY);
    }

    base_type = m->base_monster;
    if (base_type == MONS_NO_MONSTER)
        base_type = type;

    if (type == MONS_SLIME_CREATURE)
        slime_size = m->blob_size;
    else if (type == MONS_BALLISTOMYCETE)
        is_active = !!m->ballisto_activity;
    else if (_has_hydra_multi_attack(*this))
        num_heads = m->num_heads;
    // others use number for internal information
    else
        number = 0;

    _colour = m->colour;

    if (m->is_summoned()
        && (!m->has_ench(ENCH_PHANTOM_MIRROR) || m->friendly()))
    {
        mb.set(MB_SUMMONED);
    }
    else if (m->is_perm_summoned())
        mb.set(MB_PERM_SUMMON);
    else if (testbits(m->flags, MF_NO_REWARD)
             && mons_class_gives_xp(m->type, true))
    {
        mb.set(MB_NO_REWARD);
    }

    if (m->has_ench(ENCH_SUMMON_CAPPED))
        mb.set(MB_SUMMONED_CAPPED);

    if (mons_is_unique(type))
    {
        if (type == MONS_LERNAEAN_HYDRA
            || type == MONS_ROYAL_JELLY
            || mons_species(type) == MONS_SERPENT_OF_HELL)
        {
            mb.set(MB_NAME_THE);
        }
        else
        {
            mb.set(MB_NAME_UNQUALIFIED);
            mb.set(MB_NAME_THE);
        }
    }

    mname = m->mname;

    const auto name_flags = m->flags & MF_NAME_MASK;

    if (name_flags == MF_NAME_SUFFIX)
        mb.set(MB_NAME_SUFFIX);
    else if (name_flags == MF_NAME_ADJECTIVE)
        mb.set(MB_NAME_ADJECTIVE);
    else if (name_flags == MF_NAME_REPLACE)
        mb.set(MB_NAME_REPLACE);

    const bool need_name_desc =
        name_flags == MF_NAME_SUFFIX
            || name_flags == MF_NAME_ADJECTIVE
            || (m->flags & MF_NAME_DEFINITE);

    if (!mname.empty()
        && !(m->flags & MF_NAME_DESCRIPTOR)
        && !need_name_desc)
    {
        mb.set(MB_NAME_UNQUALIFIED);
        mb.set(MB_NAME_THE);
    }
    else if (m->flags & MF_NAME_DEFINITE)
        mb.set(MB_NAME_THE);
    if (m->flags & MF_NAME_ZOMBIE)
        mb.set(MB_NAME_ZOMBIE);
    if (m->flags & MF_NAME_SPECIES)
        mb.set(MB_NO_NAME_TAG);

    // Ghostliness needed for name
    if (testbits(m->flags, MF_SPECTRALISED))
        mb.set(MB_SPECTRALISED);

    if (milev <= MILEV_NAME)
    {
        if (mons_class_is_animated_weapon(type))
        {
            if (m->get_defining_object())
            {
                inv[MSLOT_WEAPON].reset(new item_def(
                    get_item_known_info(*m->get_defining_object())));
            }
            // animated launchers may have a missile too
            if (m->inv[MSLOT_MISSILE] != NON_ITEM)
            {
                inv[MSLOT_MISSILE].reset(new item_def(
                    get_item_known_info(env.item[m->inv[MSLOT_MISSILE]])));
            }
        }
        else if (type == MONS_ANIMATED_ARMOUR && m->get_defining_object())
        {
            inv[MSLOT_ARMOUR].reset(new item_def(
                get_item_known_info(*m->get_defining_object())));
        }
        return;
    }

    holi = m->holiness();

    mintel = mons_intel(*m);
    hd = m->get_hit_dice();
    ac = m->armour_class();
    ev = m->evasion();
    base_ev = m->base_evasion();
    mr = m->willpower();
    can_see_invis = m->can_see_invisible();
    if (m->nightvision())
        props[NIGHTVISION_KEY] = true;
    mresists = get_mons_resists(*m);
    mitemuse = mons_itemuse(*m);
    mbase_speed = mons_base_speed(*m, true);
    menergy = mons_energy(*m);
    can_go_frenzy = m->can_go_frenzy();
    can_feel_fear = m->can_feel_fear(false);

    // Not an MB_ because it's rare.
    if (m->cloud_immune())
        props[CLOUD_IMMUNE_MB_KEY] = true;

    if (m->airborne())
        mb.set(MB_AIRBORNE);
    if (mons_wields_two_weapons(*m))
        mb.set(MB_TWO_WEAPONS);
    if (!mons_can_regenerate(*m))
        mb.set(MB_NO_REGEN);
    if (m->haloed() && !m->umbraed())
        mb.set(MB_HALOED);
    if (!m->haloed() && m->umbraed())
        mb.set(MB_UMBRAED);
    if (mons_looks_stabbable(*m))
        mb.set(MB_STABBABLE);
    if (mons_looks_distracted(*m))
        mb.set(MB_DISTRACTED);
    if (m->liquefied_ground())
        mb.set(MB_SLOW_MOVEMENT);
    if (!actor_is_susceptible_to_vampirism(*m))
        mb.set(MB_CANT_DRAIN);

    dam = mons_get_damage_level(*m);

    // BEH_SLEEP is meaningless on firewood, don't show it. But it *is*
    // meaningful on non-firewood non-threatening monsters (i.e. butterflies).
    if (!mons_is_firewood(*m) && m->asleep())
    {
        if (!m->can_hibernate(true))
            mb.set(MB_DORMANT);
        else
            mb.set(MB_SLEEPING);
    }
    else if (mons_is_threatening(*m))
    {
        // Applies to both friendlies and hostiles
        if (mons_is_fleeing(*m))
            mb.set(MB_FLEEING);
        else if (mons_is_wandering(*m) && !mons_is_batty(*m))
        {
            if (m->is_stationary())
                mb.set(MB_UNAWARE);
            else
                mb.set(MB_WANDERING);
        }
        else if (m->foe == MHITNOT
                 && !mons_is_batty(*m)
                 && m->attitude == ATT_HOSTILE)
        {
            mb.set(MB_UNAWARE);
        }
    }

    for (auto &entry : m->enchantments)
    {
        monster_info_flags flag = ench_to_mb(*m, entry.first);
        if (flag != NUM_MB_FLAGS)
            mb.set(flag);
    }

    if (type == MONS_SILENT_SPECTRE)
        mb.set(MB_SILENCING);

    if (you.beheld_by(*m))
        mb.set(MB_MESMERIZING);

    if (you.afraid_of(m))
        mb.set(MB_FEAR_INSPIRING);

    if (testbits(m->flags, MF_ENSLAVED_SOUL))
        mb.set(MB_ENSLAVED);

    if (m->is_shapeshifter() && (m->flags & MF_KNOWN_SHIFTER))
        mb.set(MB_SHAPESHIFTER);

    if (m->known_chaos())
        mb.set(MB_CHAOTIC);

    if (m->submerged())
        mb.set(MB_SUBMERGED);

    if (m->type == MONS_DOOM_HOUND && !m->props.exists(DOOM_HOUND_HOWLED_KEY)
        && !m->is_summoned())
    {
        mb.set(MB_READY_TO_HOWL);
    }

    if (mons_is_pghost(type))
    {
        ASSERT(m->ghost);
        ghost_demon& ghost = *m->ghost;
        i_ghost.species = ghost.species;
        if (species::is_draconian(i_ghost.species) && ghost.xl < 7)
            i_ghost.species = SP_BASE_DRACONIAN;
        i_ghost.job = ghost.job;
        i_ghost.religion = ghost.religion;
        i_ghost.best_skill = ghost.best_skill;
        i_ghost.best_skill_rank = get_skill_rank(ghost.best_skill_level);
        i_ghost.xl_rank = ghost_level_to_rank(ghost.xl);
        i_ghost.ac = quantise(ghost.ac, 5);
        i_ghost.damage = ghost.damage;
        props[KNOWN_MAX_HP_KEY] = (int)ghost.max_hp;
        if (m->props.exists(MIRRORED_GHOST_KEY))
            props[MIRRORED_GHOST_KEY] = m->props[MIRRORED_GHOST_KEY];
    }
    if (m->has_ghost_brand())
        props[SPECIAL_WEAPON_KEY] = m->ghost_brand();

    // book loading for player ghost and vault monsters
    spells.clear();
    if (m->props.exists(CUSTOM_SPELLS_KEY) || mons_is_pghost(type)
        || type == MONS_PANDEMONIUM_LORD)
    {
        spells = m->spells;
    }

    if (m->is_priest())
        props[PRIEST_KEY] = true;
    else if (m->is_actual_spellcaster())
        props[ACTUAL_SPELLCASTER_KEY] = true;

    // assumes spell hd modifying effects are always public
    const int spellhd = m->spell_hd();
    if (spellhd != hd)
        props[SPELL_HD_KEY] = spellhd;

    for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
    {
        // hydras are a mess!
        const int atk_index = m->has_hydra_multi_attack() ? i + m->heads() - 1
                                                          : i;
        attack[i] = mons_attack_spec(*m, atk_index, true);
    }

    for (unsigned i = 0; i <= MSLOT_LAST_VISIBLE_SLOT; ++i)
    {
        bool ok;
        if (m->inv[i] == NON_ITEM)
            ok = false;
        else if (i == MSLOT_MISCELLANY)
            ok = false;
        else if (i == MSLOT_WAND && !m->likes_wand(env.item[m->inv[i]]))
            ok = false;
        else if (attitude == ATT_FRIENDLY)
            ok = true;
        else if (i == MSLOT_ALT_WEAPON)
            ok = wields_two_weapons();
        else
            ok = true;
        if (ok)
        {
            inv[i].reset(
                new item_def(get_item_known_info(env.item[m->inv[i]])));
            if (i == MSLOT_MISSILE && inv[i]->sub_type != MI_THROWING_NET)
                inv[i]->quantity = 1;
        }
    }

    fire_blocker = DNGN_UNSEEN;
    if (!crawl_state.arena_suspended
        && m->pos() != you.pos())
    {
        _blocked_ray(m->pos(), &fire_blocker);
    }

    // init names of constrictor and constrictees
    constrictor_name = "";
    constricting_name.clear();

    // Name of what this monster is directly constricted by, if any
    if (m->is_directly_constricted())
    {
        const actor * const constrictor = actor_by_mid(m->constricted_by);
        ASSERT(constrictor);
        constrictor_name = (constrictor->constriction_does_damage(true) ?
                            "constricted by " : "held by ")
                           + constrictor->name(_article_for(constrictor),
                                               true);
    }

    // Names of what this monster is directly constricting, if any
    if (m->constricting)
    {
        const char *participle =
            m->constriction_does_damage(true) ? "constricting " : "holding ";
        for (const auto &entry : *m->constricting)
        {
            const actor* const constrictee = actor_by_mid(entry.first);

            if (constrictee && constrictee->is_directly_constricted())
            {
                constricting_name.push_back(participle
                                            + constrictee->name(
                                                  _article_for(constrictee),
                                                  true));
            }
        }
    }

    if (mons_has_ranged_attack(*m))
        mb.set(MB_RANGED_ATTACK);

    if (is_ally_target(*m))
        mb.set(MB_ALLY_TARGET);

    // this must be last because it provides this structure to Lua code
    if (milev > MILEV_SKIP_SAFE)
    {
        if (mons_is_safe(m))
            mb.set(MB_SAFE);
        else
            mb.set(MB_UNSAFE);
        if (mons_is_firewood(*m))
            mb.set(MB_FIREWOOD);
    }

    client_id = m->get_client_id();
}

/// Player-known max HP information for a monster: "about 55", "243".
string monster_info::get_max_hp_desc() const
{
    if (props.exists(KNOWN_MAX_HP_KEY))
        return std::to_string(props[KNOWN_MAX_HP_KEY].get_int());

    const int scale = 100;
    const int base_avg_hp = mons_class_is_zombified(type) ?
                            derived_undead_avg_hp(type, hd, scale) :
                            mons_avg_hp(type, scale);
    int mhp = base_avg_hp;
    if (props.exists(VAULT_HD_KEY))
    {
        const int xl = props[VAULT_HD_KEY].get_int();
        const int base_xl = mons_class_hit_dice(type);
        mhp = base_avg_hp * xl / base_xl; // rounds down - close enough
    }

    if (type == MONS_SLIME_CREATURE)
        mhp *= slime_size;

    mhp /= scale;
    return make_stringf("about %d", mhp);
}

/**
 * Calculate some defender-specific effects on an attacker's to-hit.
 */
int monster_info::lighting_modifiers() const
{
    // Lighting effects.
    if (is(MB_GLOWING)       // corona, silver corona (!)
        || is(MB_BURNING)    // sticky flame
        || is(MB_HALOED))
    {
        return BACKLIGHT_TO_HIT_BONUS;
    }
    if (is(MB_UMBRAED) && !you.nightvision())
        return UMBRA_TO_HIT_MALUS;

    return 0;
}


/**
 * Name the given mutant beast tier.
 *
 * @param xl_tier   The beast_tier in question.
 * @return          The name of the tier; e.g. "juvenile".
 */
static string _mutant_beast_tier_name(short xl_tier)
{
    if (xl_tier < 0 || xl_tier >= NUM_BEAST_TIERS)
        return "buggy";
    return mutant_beast_tier_names[xl_tier];
}

/**
 * Name the given mutant beast facet.
 *
 * @param xl_tier   The beast_facet in question.
 * @return          The name of the facet; e.g. "bat".
 */
static string _mutant_beast_facet(int facet)
{
    if (facet < 0 || facet >= NUM_BEAST_FACETS)
        return "buggy";
    return mutant_beast_facet_names[facet];
}


string monster_info::db_name() const
{
    if (type == MONS_DANCING_WEAPON && inv[MSLOT_WEAPON])
    {
        iflags_t ignore_flags = ISFLAG_KNOW_PLUSES;
        bool     use_inscrip  = false;
        return inv[MSLOT_WEAPON]->name(DESC_DBNAME, false, false, use_inscrip, false,
                         ignore_flags);
    }

    if (type == MONS_SENSED)
        return get_monster_data(base_type)->name;

    return get_monster_data(type)->name;
}

string monster_info::_core_name() const
{
    monster_type nametype = type;

    if (mons_class_is_zombified(type))
    {
        if (mons_is_unique(base_type))
            nametype = mons_species(base_type);
        else
            nametype = base_type;
    }
    else if (type == MONS_PILLAR_OF_SALT
             || type == MONS_BLOCK_OF_ICE
             || type == MONS_SENSED)
    {
        nametype = base_type;
    }

    string s;

    if (is(MB_NAME_REPLACE))
        s = mname;
    else if (nametype == MONS_LERNAEAN_HYDRA)
        s = "Lernaean hydra"; // TODO: put this into mon-data.h
    else if (nametype == MONS_ROYAL_JELLY)
        s = "Royal Jelly";
    else if (mons_species(nametype) == MONS_SERPENT_OF_HELL)
        s = "Serpent of Hell";
    else if (invalid_monster_type(nametype) && nametype != MONS_PROGRAM_BUG)
        s = "INVALID MONSTER";
    else
    {
        const char* slime_sizes[] = {"buggy ", "", "large ", "very large ",
                                               "enormous ", "titanic "};
        s = get_monster_data(nametype)->name;

        if (mons_is_draconian_job(type) && base_type != MONS_NO_MONSTER)
            s = draconian_colour_name(base_type) + " " + s;

        switch (type)
        {
        case MONS_SLIME_CREATURE:
            ASSERT((size_t) slime_size <= ARRAYSZ(slime_sizes));
            s = slime_sizes[slime_size] + s;
            break;
        case MONS_UGLY_THING:
        case MONS_VERY_UGLY_THING:
            s = ugly_thing_colour_name(_colour) + " " + s;
            break;

        case MONS_DANCING_WEAPON:
        case MONS_SPECTRAL_WEAPON:
            if (inv[MSLOT_WEAPON])
            {
                const item_def& item = *inv[MSLOT_WEAPON];
                s = item.name(DESC_PLAIN, false, false, true, false);
            }
            break;

        case MONS_ANIMATED_ARMOUR:
            if (inv[MSLOT_ARMOUR])
            {
                const item_def& item = *inv[MSLOT_ARMOUR];
                s = "animated " + item.name(DESC_PLAIN, false, false, true, false, ISFLAG_KNOW_PLUSES);
            }
            break;

        case MONS_PLAYER_GHOST:
            s = apostrophise(mname) + " ghost";
            break;
        case MONS_PLAYER_ILLUSION:
            s = apostrophise(mname) + " illusion";
            break;
        case MONS_PANDEMONIUM_LORD:
            s = mname;
            break;
        case MONS_LIVING_SPELL:
            if (has_spells())
            {
                switch (spells[0].spell)
                {
                case SPELL_BANISHMENT:
                    s = "living banishment spell";
                    break;
                case SPELL_LEHUDIBS_CRYSTAL_SPEAR:
                    s = "living crystal spell";
                    break;
                case SPELL_SMITING:
                    s = "living smiting commandment";
                    break;
                case SPELL_PARALYSE:
                    s = "living paralysis spell";
                    break;
                default:
                    break;
                }
            }
            break;
        default:
            break;
        }
    }

    //XXX: Hack to get poly'd TLH's name on death to look right.
    if (is(MB_NAME_SUFFIX) && type != MONS_LERNAEAN_HYDRA)
        s += " " + mname;
    else if (is(MB_NAME_ADJECTIVE))
        s = mname + " " + s;

    return s;
}

string monster_info::_apply_adjusted_description(description_level_type desc,
                                                 const string& s) const
{
    if (desc == DESC_ITS)
        desc = DESC_THE;

    if (is(MB_NAME_THE) && desc == DESC_A)
        desc = DESC_THE;

    if (attitude == ATT_FRIENDLY && desc == DESC_THE)
        desc = DESC_YOUR;

    return apply_description(desc, s);
}

string monster_info::common_name(description_level_type desc) const
{
    const string core = _core_name();
    const bool nocore = mons_class_is_zombified(type)
                          && mons_is_unique(base_type)
                          && base_type == mons_species(base_type)
                        || type == MONS_MUTANT_BEAST && !is(MB_NAME_REPLACE);

    ostringstream ss;

    if (props.exists(HELPLESS_KEY))
        ss << "helpless ";

    if (is(MB_SUBMERGED))
        ss << "submerged ";

    if (type == MONS_SPECTRAL_THING && !is(MB_NAME_ZOMBIE) && !nocore)
        ss << "spectral ";

    if (is(MB_SPECTRALISED))
        ss << "ghostly ";

    if (type == MONS_SENSED && !mons_is_sensed(base_type))
        ss << "sensed ";

    if (type == MONS_BALLISTOMYCETE)
        ss << (is_active ? "active " : "");

    if (_has_hydra_multi_attack(*this)
        && type != MONS_SENSED
        && type != MONS_BLOCK_OF_ICE
        && type != MONS_PILLAR_OF_SALT
        && mons_species(type) != MONS_SERPENT_OF_HELL)
    {
        ASSERT(num_heads > 0);
        if (num_heads < 11)
            ss << number_in_words(num_heads);
        else
            ss << std::to_string(num_heads);

        ss << "-headed ";
    }

    if (type == MONS_MUTANT_BEAST && !is(MB_NAME_REPLACE))
    {
        const int xl = props[MUTANT_BEAST_TIER].get_short();
        const int tier = mutant_beast_tier(xl);
        ss << _mutant_beast_tier_name(tier) << " ";
        for (auto facet : props[MUTANT_BEAST_FACETS].get_vector())
            ss << _mutant_beast_facet(facet.get_int()); // no space between
        ss << " beast";
    }

    if (!nocore)
        ss << core;

    // Add suffixes.
    switch (type)
    {
    case MONS_ZOMBIE:
#if TAG_MAJOR_VERSION == 34
    case MONS_ZOMBIE_SMALL:
    case MONS_ZOMBIE_LARGE:
#endif
        if (!is(MB_NAME_ZOMBIE))
            ss << (nocore ? "" : " ") << "zombie";
        break;
    case MONS_SKELETON:
#if TAG_MAJOR_VERSION == 34
    case MONS_SKELETON_SMALL:
    case MONS_SKELETON_LARGE:
#endif
        if (!is(MB_NAME_ZOMBIE))
            ss << (nocore ? "" : " ") << "skeleton";
        break;
    case MONS_SIMULACRUM:
#if TAG_MAJOR_VERSION == 34
    case MONS_SIMULACRUM_SMALL:
    case MONS_SIMULACRUM_LARGE:
#endif
        if (!is(MB_NAME_ZOMBIE))
            ss << (nocore ? "" : " ") << "simulacrum";
        break;
    case MONS_SPECTRAL_THING:
        if (nocore)
            ss << "spectre";
        break;
    case MONS_PILLAR_OF_SALT:
        ss << (nocore ? "" : " ") << "shaped pillar of salt";
        break;
    case MONS_BLOCK_OF_ICE:
        ss << (nocore ? "" : " ") << "shaped block of ice";
        break;
    default:
        break;
    }

    if (is(MB_SHAPESHIFTER))
    {
        // If momentarily in original form, don't display "shaped
        // shifter".
        if (mons_genus(type) != MONS_SHAPESHIFTER)
            ss << " shaped shifter";
    }

    string s;
    // only respect unqualified if nothing was added ("Sigmund" or "The spectral Sigmund")
    if (!is(MB_NAME_UNQUALIFIED) || has_proper_name() || ss.str() != core)
        s = _apply_adjusted_description(desc, ss.str());
    else
        s = ss.str();

    if (desc == DESC_ITS)
        s = apostrophise(s);

    return s;
}

bool monster_info::has_proper_name() const
{
    return !mname.empty() && !mons_is_ghost_demon(type)
            && !is(MB_NAME_REPLACE) && !is(MB_NAME_ADJECTIVE) && !is(MB_NAME_SUFFIX);
}

string monster_info::proper_name(description_level_type desc) const
{
    if (has_proper_name())
    {
        if (desc == DESC_ITS)
            return apostrophise(mname);
        else
            return mname;
    }
    else
        return common_name(desc);
}

string monster_info::full_name(description_level_type desc) const
{
    if (desc == DESC_NONE)
        return "";

    if (has_proper_name())
    {
        string s = mname + " the " + common_name();
        if (desc == DESC_ITS)
            s = apostrophise(s);
        return s;
    }
    else
        return common_name(desc);
}

// Needed because gcc 4.3 sort does not like comparison functions that take
// more than 2 arguments.
bool monster_info::less_than_wrapper(const monster_info& m1,
                                     const monster_info& m2)
{
    return monster_info::less_than(m1, m2, true);
}

// Sort monsters by (in that order):    attitude, difficulty, type, brand
bool monster_info::less_than(const monster_info& m1, const monster_info& m2,
                             bool zombified, bool fullname)
{
    // This awkward ordering (checking m2 before m1) is required to satisfy
    // std::sort's contract. Specifically, if m1 and m2 are both ancestors
    // (e.g. through phantom mirror), we want them to compare equal. To signify
    // "equal", we must say that neither is less than the other, rather than
    // saying that both are less than each other.
    if (mons_is_hepliaklqana_ancestor(m2.type))
        return false;
    else if (mons_is_hepliaklqana_ancestor(m1.type))
        return true;

    if (m1.attitude < m2.attitude)
        return true;
    else if (m1.attitude > m2.attitude)
        return false;

    // Force plain but different coloured draconians to be treated like the
    // same sub-type.
    if (!zombified
        && mons_is_base_draconian(m1.type)
        && mons_is_base_draconian(m2.type))
    {
        return false;
    }

    int diff_delta = mons_avg_hp(m1.type) - mons_avg_hp(m2.type);

    // By descending difficulty
    if (diff_delta > 0)
        return true;
    else if (diff_delta < 0)
        return false;

    if (m1.type < m2.type)
        return true;
    else if (m1.type > m2.type)
        return false;

    // Never distinguish between dancing weapons.
    // The above checks guarantee that *both* monsters are of this type.
    if (m1.type == MONS_DANCING_WEAPON || m1.type == MONS_ANIMATED_ARMOUR)
        return false;

    if (m1.type == MONS_SLIME_CREATURE)
        return m1.slime_size > m2.slime_size;

    if (m1.type == MONS_BALLISTOMYCETE)
        return m1.is_active && !m2.is_active;

    // Shifters after real monsters of the same type.
    if (m1.is(MB_SHAPESHIFTER) != m2.is(MB_SHAPESHIFTER))
        return m2.is(MB_SHAPESHIFTER);

    // Spectralised after the still-living. There's not terribly much
    // difference, but this keeps us from combining them in the monster
    // list so they all appear to be spectralised.
    if (m1.is(MB_SPECTRALISED) != m2.is(MB_SPECTRALISED))
        return m2.is(MB_SPECTRALISED);

    if (zombified)
    {
        if (mons_class_is_zombified(m1.type))
        {
            // Because of the type checks above, if one of the two is zombified, so
            // is the other, and of the same type.
            if (m1.base_type < m2.base_type)
                return true;
            else if (m1.base_type > m2.base_type)
                return false;
        }

        // Both monsters are hydras or hydra zombies, sort by number of heads.
        if (_has_hydra_multi_attack(m1))
        {
            if (m1.num_heads > m2.num_heads)
                return true;
            else if (m1.num_heads < m2.num_heads)
                return false;
        }
    }

    if (fullname || mons_is_pghost(m1.type))
        return m1.mname < m2.mname;

    return false;
}

static string _verbose_info(const monster_info& mi)
{
    string desc= "";
    vector<monster_info> miv;
    miv.push_back(mi);
    mons_conditions_string(desc, miv, 0, 1, false);
    return desc;
}

string monster_info::pluralised_name(bool fullname) const
{
    // Don't pluralise uniques, ever. Multiple copies of the same unique
    // are unlikely in the dungeon currently, but quite common in the
    // arena. This prevents "4 Gra", etc. {due}
    // Unless it's Mara, who summons illusions of himself.
    if (mons_is_unique(type) && type != MONS_MARA)
        return common_name();
    else if (mons_genus(type) == MONS_DRACONIAN)
        return pluralise_monster(mons_type_name(MONS_DRACONIAN, DESC_PLAIN));
    else if (type == MONS_UGLY_THING || type == MONS_VERY_UGLY_THING
             || type == MONS_DANCING_WEAPON || type == MONS_SPECTRAL_WEAPON
             || type == MONS_ANIMATED_ARMOUR || type == MONS_MUTANT_BEAST
             || !fullname)
    {
        return pluralise_monster(mons_type_name(type, DESC_PLAIN));
    }
    else
        return pluralise_monster(common_name());
}

enum _monster_list_colour_type
{
    _MLC_FRIENDLY, _MLC_NEUTRAL, _MLC_GOOD_NEUTRAL, _MLC_STRICT_NEUTRAL,
    _MLC_TRIVIAL, _MLC_EASY, _MLC_TOUGH, _MLC_NASTY,
    _NUM_MLC
};

static const char * const _monster_list_colour_names[_NUM_MLC] =
{
    "friendly", "neutral", "good_neutral", "strict_neutral",
    "trivial", "easy", "tough", "nasty"
};

static int _monster_list_colours[_NUM_MLC] =
{
    GREEN, BROWN, BROWN, BROWN,
    DARKGREY, LIGHTGREY, YELLOW, LIGHTRED,
};

bool set_monster_list_colour(string key, int colour)
{
    for (int i = 0; i < _NUM_MLC; ++i)
    {
        if (key == _monster_list_colour_names[i])
        {
            _monster_list_colours[i] = colour;
            return true;
        }
    }
    return false;
}

void clear_monster_list_colours()
{
    for (int i = 0; i < _NUM_MLC; ++i)
        _monster_list_colours[i] = -1;
}

void monster_info::to_string(int count, string& desc, int& desc_colour,
                             bool fullname, const char *adj,
                             bool verbose) const
{
    ostringstream out;
    _monster_list_colour_type colour_type = _NUM_MLC;

    string full = count == 1 ? full_name() : pluralised_name(fullname);

    if (adj && starts_with(full, "the "))
        full.erase(0, 4);

    // TODO: this should be done in a much cleaner way, with code to
    // merge multiple monster_infos into a single common structure
    if (count != 1)
        out << count << " ";
    if (adj)
        out << adj << " ";
    out << full;

#ifdef DEBUG_DIAGNOSTICS
    out << " av" << mons_avg_hp(type);
#endif

    if (count == 1 && verbose)
       out << _verbose_info(*this);

    // Friendliness
    switch (attitude)
    {
    case ATT_FRIENDLY:
        //out << " (friendly)";
        colour_type = _MLC_FRIENDLY;
        break;
    case ATT_GOOD_NEUTRAL:
        //out << " (neutral)";
        colour_type = _MLC_GOOD_NEUTRAL;
        break;
    case ATT_NEUTRAL:
        //out << " (neutral)";
        colour_type = _MLC_NEUTRAL;
        break;
    case ATT_STRICT_NEUTRAL:
        out << " (fellow slime)";
        colour_type = _MLC_STRICT_NEUTRAL;
        break;
    case ATT_HOSTILE:
        // out << " (hostile)";
        switch (threat)
        {
        case MTHRT_TRIVIAL: colour_type = _MLC_TRIVIAL; break;
        case MTHRT_EASY:    colour_type = _MLC_EASY;    break;
        case MTHRT_TOUGH:   colour_type = _MLC_TOUGH;   break;
        case MTHRT_NASTY:   colour_type = _MLC_NASTY;   break;
        default:;
        }
        break;
    }

    if (colour_type < _NUM_MLC)
        desc_colour = _monster_list_colours[colour_type];

    // We still need something, or we'd get the last entry's colour.
    if (desc_colour < 0)
        desc_colour = LIGHTGREY;

    desc = out.str();
}

vector<string> monster_info::attributes() const
{
    vector<string> v;
    for (auto& name : monster_info_flag_names)
    {
        if (is(name.flag))
        {
            // TODO: just use `do_mon_str_replacements`?
            v.push_back(replace_all(name.long_singular,
                                    "@possessive@",
                                    pronoun(PRONOUN_POSSESSIVE)));
        }
    }

    return v;
}

string monster_info::wounds_description_sentence() const
{
    const string wounds = wounds_description();
    if (wounds.empty())
        return "";
    else
    {
        return string(pronoun(PRONOUN_SUBJECTIVE)) + " "
               + conjugate_verb("are", pronoun_plurality())
               + " " + wounds + ".";
    }
}

string monster_info::wounds_description(bool use_colour) const
{
    if (dam == MDAM_OKAY)
        return "";

    string desc = get_damage_level_string(holi, dam);
    if (use_colour)
    {
        const int col = channel_to_colour(MSGCH_MONSTER_DAMAGE, dam);
        desc = colour_string(desc, col);
    }
    return desc;
}

string monster_info::constriction_description() const
{
    string cinfo = "";
    bool bymsg = false;

    if (!constrictor_name.empty())
    {
        cinfo += constrictor_name;
        bymsg = true;
    }

    string constricting = comma_separated_line(constricting_name.begin(),
                                               constricting_name.end());

    if (!constricting.empty())
    {
        if (bymsg)
            cinfo += ", ";
        cinfo += constricting;
    }
    return cinfo;
}

int monster_info::randarts(artefact_prop_type ra_prop) const
{
    int ret = 0;

    if (itemuse() >= MONUSE_STARTING_EQUIPMENT)
    {
        item_def* weapon = inv[MSLOT_WEAPON].get();
        item_def* second = inv[MSLOT_ALT_WEAPON].get(); // Two-headed ogres, etc.
        item_def* armour = inv[MSLOT_ARMOUR].get();
        item_def* shield = inv[MSLOT_SHIELD].get();
        item_def* ring   = inv[MSLOT_JEWELLERY].get();

        if (weapon && weapon->base_type == OBJ_WEAPONS && is_artefact(*weapon))
            ret += artefact_property(*weapon, ra_prop);

        if (second && second->base_type == OBJ_WEAPONS && is_artefact(*second))
            ret += artefact_property(*second, ra_prop);

        if (armour && armour->base_type == OBJ_ARMOUR && is_artefact(*armour))
            ret += artefact_property(*armour, ra_prop);

        if (shield && shield->base_type == OBJ_ARMOUR && is_artefact(*shield))
            ret += artefact_property(*shield, ra_prop);

        if (ring && ring->base_type == OBJ_JEWELLERY && is_artefact(*ring))
            ret += artefact_property(*ring, ra_prop);
    }

    return ret;
}

/**
 * Can the monster described by this monster_info see invisible creatures?
 */
bool monster_info::can_see_invisible() const
{
    return can_see_invis;
}

/**
 * Does the monster described by this monster_info ignore umbra acc penalties?
 */
bool monster_info::nightvision() const
{
    return props.exists(NIGHTVISION_KEY);
}

int monster_info::willpower() const
{
    return mr;
}

string monster_info::speed_description() const
{
    if (mbase_speed < 7)
        return "very slow";
    else if (mbase_speed < 10)
        return "slow";
    else if (mbase_speed > 20)
        return "extremely fast";
    else if (mbase_speed > 15)
        return "very fast";
    else if (mbase_speed > 10)
        return "fast";

    // This only ever displays through Lua.
    return "normal";
}

bool monster_info::wields_two_weapons() const
{
    return is(MB_TWO_WEAPONS);
}

bool monster_info::can_regenerate() const
{
    return !is(MB_NO_REGEN);
}

reach_type monster_info::reach_range(bool items) const
{
    const monsterentry *e = get_monster_data(mons_class_is_zombified(type)
                                             ? base_type : type);
    ASSERT(e);
    reach_type range = REACH_NONE;

    for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
    {
        const attack_flavour fl = e->attack[i].flavour;
        if (flavour_has_reach(fl))
            range = REACH_TWO;
    }

    if (items)
    {
        const item_def *weapon = inv[MSLOT_WEAPON].get();
        if (weapon)
            range = max(range, weapon_reach(*weapon));
    }

    return range;
}

size_type monster_info::body_size() const
{
    const size_type class_size = mons_class_body_size(base_type);

    // Slime creature size is increased by the number merged.
    if (type == MONS_SLIME_CREATURE)
    {
        if (slime_size == 2)
            return SIZE_MEDIUM;
        else if (slime_size == 3)
            return SIZE_LARGE;
        else if (slime_size >= 4) // sizes 4 & 5
            return SIZE_GIANT;
    }

    return class_size;
}

bool monster_info::cannot_move() const
{
    return is(MB_PARALYSED) || is(MB_PETRIFIED);
}

bool monster_info::airborne() const
{
    return is(MB_AIRBORNE);
}

bool monster_info::ground_level() const
{
    return !airborne();
}

// Only checks for spells from preset monster spellbooks.
// Use monster.h's has_spells for knowing a monster has spells
bool monster_info::has_spells() const
{
    // Some monsters have a special book but may not have any spells anyways.
    if (props.exists(CUSTOM_SPELLS_KEY))
        return spells.size() > 0 && spells[0].spell != SPELL_NO_SPELL;

    // Almost all draconians have breath spells.
    if (mons_genus(draconian_subspecies()) == MONS_DRACONIAN
        && draconian_subspecies() != MONS_GREY_DRACONIAN
        && draconian_subspecies() != MONS_DRACONIAN)
    {
        return true;
    }

    const mon_spellbook_type book = get_spellbook(*this);

    if (book == MST_NO_SPELLS)
        return false;

    // Ghosts / pan lords may have custom spell lists, so check spells directly
    if (book == MST_GHOST || type == MONS_PANDEMONIUM_LORD)
        return spells.size() > 0;

    return true;
}

/// What hd does this monster cast spells with? May vary from actual HD.
int monster_info::spell_hd(spell_type spell) const
{
    UNUSED(spell);
    if (!props.exists(SPELL_HD_KEY))
        return hd;
    return props[SPELL_HD_KEY].get_int();
}

unsigned monster_info::colour(bool base_colour) const
{
    if (!base_colour && Options.mon_glyph_overrides.count(type)
        && Options.mon_glyph_overrides[type].col)
    {
        return Options.mon_glyph_overrides[type].col;
    }
    else if (_colour == COLOUR_INHERIT)
        return mons_class_colour(type);
    else
    {
        ASSERT_RANGE(_colour, 0, NUM_COLOURS);
        return _colour;
    }
}

void monster_info::set_colour(int col)
{
    ASSERT_RANGE(col, -1, NUM_COLOURS);
    _colour = col;
}

/**
 * Does this monster have the given enchantment,
 * to the best of the player's knowledge?
 *
 * Only handles trivially mapped MBs.
 */
bool monster_info::has_trivial_ench(enchant_type ench) const
{
    monster_info_flags *flag = map_find(trivial_ench_mb_mappings, ench);
    return flag && is(*flag);
}

/// Can this monster be debuffed, to the best of the player's knowledge?
bool monster_info::debuffable() const
{
    // NOTE: assumes that all debuffable enchantments are trivially mapped
    // to MBs.

    // can't debuff innately invisible monsters
    if (is(MB_INVISIBLE) && !mons_class_flag(type, M_INVIS))
        return true;

    return any_of(begin(dispellable_enchantments),
                  end(dispellable_enchantments),
                  [this](enchant_type ench) -> bool
                  { return this->has_trivial_ench(ench); });
}

void get_monster_info(vector<monster_info>& mons)
{
    vector<monster* > visible;
    if (crawl_state.game_is_arena())
    {
        for (monster_iterator mi; mi; ++mi)
            visible.push_back(*mi);
    }
    else
        visible = get_nearby_monsters();

    for (monster *mon : visible)
    {
        if (mons_is_threatening(*mon)
            || mon->is_child_tentacle())
        {
            mons.emplace_back(mon);
        }
    }
    sort(mons.begin(), mons.end(), monster_info::less_than_wrapper);
}

void mons_to_string_pane(string& desc, int& desc_colour, bool fullname,
                         const vector<monster_info>& mi, int start,
                         int count)
{
    mi[start].to_string(count, desc, desc_colour, fullname, nullptr, false);
    mons_conditions_string(desc, mi, start, count, true);
}

static bool _has_polearm(const monster_info& mi)
{
    if (mi.itemuse() >= MONUSE_STARTING_EQUIPMENT)
    {
        const item_def* weapon = mi.inv[MSLOT_WEAPON].get();
        return weapon && weapon_reach(*weapon) >= REACH_TWO;
    }
    else
        return mi.type == MONS_DANCING_WEAPON && mi.reach_range() >= REACH_TWO;
}

static bool _has_launcher(const monster_info& mi)
{
    if (mi.itemuse() >= MONUSE_STARTING_EQUIPMENT)
    {
        const item_def* weapon = mi.inv[MSLOT_WEAPON].get();
        const item_def* missile = mi.inv[MSLOT_MISSILE].get();
        return weapon && missile && missile->launched_by(*weapon);
    }
    else
        return false;
}

static bool _has_missile(const monster_info& mi)
{
    if (mi.itemuse() >= MONUSE_STARTING_EQUIPMENT)
    {
        const item_def* missile = mi.inv[MSLOT_MISSILE].get();
        // Assume that if the monster don't pick up items they can't use.
        return missile && is_throwable(nullptr, *missile);
    }
    else
        return false;
}
static bool _has_wand(const monster_info& mi)
{
     if (mi.itemuse() >= MONUSE_STARTING_EQUIPMENT)
         return mi.inv[MSLOT_WAND].get();
     return false;
}

static bool _has_attack_flavour(const monster_info& mi, attack_flavour af)
{
    for (int i = 0; i < MAX_NUM_ATTACKS; ++i)
    {
        const mon_attack_def &attack = mi.attack[i];
        if (attack.flavour == af)
            return true;
    }
    return false;
}

static string _condition_string(int num, int count,
                                const monster_info_flag_name& name)
{
    const string& word = (1 == num) ? name.short_singular : name.plural;

    if (count == num)
        return word;
    else
        return make_stringf("%d %s", num, word.c_str());
}

void mons_conditions_string(string& desc, const vector<monster_info>& mi,
                            int start, int count, bool equipment)
{
    vector<string> conditions;

    if (equipment)
    {
        int wand_count = 0;
        int polearm_count = 0;
        int launcher_count = 0;
        int missile_count = 0;
        int reach_count = 0;
        int constrict_count = 0;
        int trample_count = 0;

        for (int j = start; j < start + count; ++j)
        {
            if (_has_wand(mi[j]))
                wand_count++;
            if (_has_polearm(mi[j]))
                polearm_count++;
            if (_has_launcher(mi[j]))
                launcher_count++;
            else if (_has_missile(mi[j]))
                missile_count++;
            if (mi[j].reach_range(false) > REACH_NONE)
                reach_count++;
            if (_has_attack_flavour(mi[j], AF_CRUSH))
                constrict_count++;
            if (_has_attack_flavour(mi[j], AF_TRAMPLE))
                trample_count++;
        }

        if (wand_count)
        {
            conditions.push_back(_condition_string(wand_count, count,
                                                   {MB_UNSAFE, "wand",
                                                    "wand", "wands"}));
        }

        if (polearm_count)
        {
            conditions.push_back(_condition_string(polearm_count, count,
                                                   {MB_UNSAFE, "polearm",
                                                    "polearm", "polearms"}));
        }

        if (launcher_count)
        {
            conditions.push_back(_condition_string(launcher_count, count,
                                                   {MB_UNSAFE, "launcher",
                                                    "launcher", "launchers"}));
        }

        if (missile_count)
        {
            conditions.push_back(_condition_string(missile_count, count,
                                                   {MB_UNSAFE, "missile",
                                                    "missile", "missiles"}));
        }

        if (reach_count)
        {
            conditions.push_back(_condition_string(reach_count, count,
                                                   {MB_UNSAFE, "reaching",
                                                    "reaching", "reaching"}));
        }

        if (constrict_count)
        {
            conditions.push_back(_condition_string(constrict_count, count,
                                                   {MB_UNSAFE, "constriction",
                                                    "constriction",
                                                    "constriction"}));
        }

        if (trample_count)
        {
            conditions.push_back(_condition_string(trample_count, count,
                                                   {MB_UNSAFE, "trample",
                                                    "trample", "trample"}));
        }
    }

    for (auto& name : monster_info_flag_names)
    {
        int num = 0;
        for (int j = start; j < start+count; j++)
        {
            if (mi[j].is(name.flag))
                num++;
        }
        if (num && !name.short_singular.empty())
            conditions.push_back(_condition_string(num, count, name));
    }


    if (conditions.empty())
        return;

    desc += " ("
         + join_strings(conditions.begin(), conditions.end(), ", ") + ")";
}

monster_type monster_info::draconian_subspecies() const
{
    if (type == MONS_PLAYER_ILLUSION && mons_genus(type) == MONS_DRACONIAN)
        return species::to_mons_species(i_ghost.species);
    return ::draconian_subspecies(type, base_type);
}

const char *monster_info::pronoun(pronoun_type variant) const
{
    if (props.exists(MON_GENDER_KEY))
    {
        return decline_pronoun((gender_type)props[MON_GENDER_KEY].get_int(),
                               variant);
    }
    return mons_pronoun(type, variant, true);
}

bool monster_info::pronoun_plurality() const
{
    if (props.exists(MON_GENDER_KEY))
        return props[MON_GENDER_KEY].get_int() == GENDER_NEUTRAL;

    return mons_class_gender(type) == GENDER_NEUTRAL;
}
