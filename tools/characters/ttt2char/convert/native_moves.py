"""Small native-moveset additions; learn wall captures from TTT2's own fighters."""
from . import tagsystem as TS
from . import idmaps as M

THROW_ATTEMPTS = ('ThrmvLB2_00', 'ThrmvRB2_00')


def wall_throw_template(fighters, forward=False):
    """Require Lili and Marduk to agree on the native wall capture's guards/timing.

    Return values, rather than table offsets: each source has private requirements
    and cancel-extra tables. Never transplant those indices into Armor King's.
    """
    templates = []
    for fighter in fighters:
        names = {fighter.move_name(i): i for i in range(len(fighter.tables['moves']))}
        victims = {i for name, i in names.items() if name.endswith('AirWallF_n')}
        if not victims:
            continue
        for base_name in THROW_ATTEMPTS:
            name = base_name + ('F' if forward else '')
            if name not in names:
                raise ValueError('wall-throw donor has no normal throw attempt')
            candidates = [r for r in M.cancels(fighter, fighter.tables['moves'][names[name]]['cancel'])
                          if r['move_id'] in victims]
            if len(candidates) != 1:
                raise ValueError('no unique native wall capture')
            row = dict(candidates[0])
            guards = M.reqs(fighter, row.pop('req'))
            mode = fighter.tables['cancel_extras'][row.pop('extradata')]['value']
            row.pop('move_id')
            templates.append((row, guards, mode))
    if len(templates) != 4 or any(t != templates[0] for t in templates[1:]):
        raise ValueError('Lili and Marduk must agree on both native wall captures')
    return templates[0]


def add_wall_throw(c, template, victim, forward=False):
    """Add a guarded capture to both native throws, preserving their existing rows."""
    row, guards, mode = template
    if not 0 <= victim < len(c.T['moves']):
        raise ValueError('wall throw victim move is missing')
    requirement = TS.write(c.T, 'requirements',
                           [dict(condition=q, param=p) for q, p in guards] +
                           [dict(condition=TS.TT_END, param=0)])
    extras = c.T['cancel_extras']
    extra = next((i for i, e in enumerate(extras) if e['value'] == mode), None)
    if extra is None:
        extra = len(extras)
        extras.append(dict(value=mode))
    attempts = [c.dn[name + ('F' if forward else '')] for name in THROW_ATTEMPTS]
    lists = {c.T['moves'][i]['cancel'] for i in attempts}
    replacements = {}
    for old in lists:
        original = [dict(r) for r in TS.cancels(c.T, old)]
        if not original or original[-1]['direction'] != M.LIST_END:
            raise ValueError('unterminated native throw attempt')
        capture = dict(row, req=requirement, extradata=extra, move_id=victim)
        replacements[old] = TS.write(c.T, 'cancels', original[:-1] + [capture, original[-1]])
    for move in c.T['moves']:
        if move['cancel'] in replacements:
            move['cancel'] = replacements[move['cancel']]
    c.stats['native wall-throw input routes added'] += len(attempts)


def air_normal_reaction(c, move, source_reaction):
    """Change airborne normal hits only, copying native hit/reaction lists first.

    CH and crouching-CH slots and their pushback remain native even while airborne;
    grounded hits retain the entire original hit-condition list.
    """
    normal = ('default', 'crouch', 'left', 'crouch_left', 'right', 'crouch_right',
              'back', 'crouch_back', 'pb_front', 'pb_back', 'pb_left', 'pb_right',
              'dir_front', 'dir_back', 'dir_left', 'dir_right', 'vertical_pushback')
    converted = c.T['reactions'][source_reaction]
    row = c.T['moves'][move]
    original = [dict(h) for h in TS.hits(c.T, row['hit_condition'])]
    if any(TS.has(c.T, h['req'], TS.AIR) for h in original):
        raise ValueError('native move already has an airborne hit-condition branch')
    air_hits = []
    for hit in original:
        reaction = dict(c.T['reactions'][hit['reactions']])
        reaction.update({field: converted[field] for field in normal})
        ri = TS.write(c.T, 'reactions', [reaction])
        guards = [(TS.AIR, 0)] + TS.reqs(c.T, hit['req'])
        req = TS.write(c.T, 'requirements', [dict(condition=q, param=p) for q, p in guards] +
                       [dict(condition=TS.TT_END, param=0)])
        air_hits.append(dict(hit, req=req, reactions=ri))
    row['hit_condition'] = TS.write(c.T, 'hit_conditions', air_hits + original)
    c.stats['native airborne normal-hit reactions changed'] += 1


def redirect_move_inputs(c, old, new):
    """Redirect move targets, never command-group offsets or list terminators."""
    changed = 0
    for table in ('cancels', 'group_cancels'):
        for row in c.T[table]:
            if row['direction'] in (M.GROUP, M.GROUP_END, M.LIST_END):
                continue
            if row['move_id'] == old:
                row['move_id'] = new
                changed += 1
    return changed


def validate_command_groups(c):
    """Group commands index group_cancels, not moves; check before serialization."""
    groups = c.T['group_cancels']
    for row in c.T['cancels']:
        if row['direction'] == M.GROUP:
            offset = row['move_id']
            if not 0 <= offset < len(groups):
                raise ValueError('native command-group offset is outside group_cancels')
            if not any(r['direction'] == M.GROUP_END for r in groups[offset:]):
                raise ValueError('native command group has no terminator')


def copy_air_reaction(c, target, source):
    """Copy a native move's explicit airborne reaction, keeping the target's damage."""
    source_move = c.T['moves'][source]
    matches = [h for h in TS.hits(c.T, source_move['hit_condition'])
               if TS.reqs(c.T, h['req']) == [(TS.AIR, 0)]]
    if len(matches) != 1:
        raise ValueError('source move has no unique airborne reaction')
    move = c.T['moves'][target]
    original = [dict(h) for h in TS.hits(c.T, move['hit_condition'])]
    air = [h for h in original if TS.reqs(c.T, h['req']) == [(TS.AIR, 0)]]
    if len(air) > 1:
        raise ValueError('target move has multiple airborne reactions')
    if air:
        air[0]['reactions'] = matches[0]['reactions']
        hits = original
    else:
        hit = dict(original[-1], req=matches[0]['req'], reactions=matches[0]['reactions'])
        hits = [hit] + original
    move['hit_condition'] = TS.write(c.T, 'hit_conditions', hits)
    c.stats['native airborne reactions copied'] += 1



def add_breath_visual(c, move_id, frames, effect, head_part):
    """Spawn the existing breath particles without the breath attack's projectile."""
    move = c.T['moves'][move_id]
    props = [dict(p) for p in TS.props(c.T, move['extraprops'])]
    for frame in frames:
        prop = dict(frame=frame, id=0x801f, value=head_part << 8 | effect)
        if prop not in props:
            props.append(prop)
    move['extraprops'] = TS.write(c.T, 'extra_move_properties', props + [dict(frame=0,id=0,value=0)])
    c.stats['native wall breath visual bursts'] += len(frames)
