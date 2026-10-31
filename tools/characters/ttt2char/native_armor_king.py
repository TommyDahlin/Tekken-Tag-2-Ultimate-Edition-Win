"""Optional T8 move additions for native Armor King (owned, locally saved game files).

Run with --game, --saved and --out. This replaces no original game archives:
outputs are loose archive overrides under the fighter-pack directory's native/aki.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace

from . import InputError, importer, source
from .convert import idmaps as M, moveset, native_moves, tagsystem as TS, weapons
from .movesets import REACTION_MOVE_FIELDS
from .tekken8 import anim, anmbin, motbin, movenames
from .ttt2 import bone, motbin as native_motbin


class Graft(moveset.Converter):
    """Append selected moves and their dependencies without rebuilding native hubs."""
    def target(self, mid, input_target=False):
        if (mid is not None and 0 <= mid < min(0x8000, len(self.cls)) and
                mid not in self.newidx and self.cls[mid] == 'K' and
                (self.alloc_ok or self.linking) and not self.drop_moves.search(self.name(mid))):
            self.newidx[mid] = len(self.T['moves'])
            self.T['moves'].append(self.conv_move(mid))
            self.queue.append(mid)
        return super().target(mid, input_target)


def named_key(k, key, length):
    found = [i for i, row in enumerate(k.tables['moves'])
             if row['name_key'] == key and k.name_lengths(i)[0] == length]
    if len(found) != 1:
        raise InputError('Armor King source changed: expected move signature %08x/%d' % (key, length))
    return found[0]


def convert(game, saved, log=print):
    saved = Path(saved)
    k = motbin.read((saved/'knk.motbin').read_bytes(),
                    (saved/'knk.motbin.fields.bin').read_bytes(), 'knk')
    archives = importer._archives(str(game))
    native = movenames.ttt2_movesets(archives)
    donor = native[29]
    if donor.name(donor.name_offsets[0]) != '[ARMOR_KING]':
        raise InputError('expected the EU v16 native Armor King moveset')
    log('Recovering Armor King move names and native conversion maps...')
    movenames.name_moves(k, native, log=log)
    refs = []
    dictionary = movenames.harvest(native)[0]
    for path in sorted(saved.glob('*.motbin')):
        if path.stem == 'knk':
            continue
        fields = path.with_suffix('.motbin.fields.bin')
        if not fields.is_file():
            continue
        other = motbin.read(path.read_bytes(), fields.read_bytes(), path.stem)
        movenames.name_known(other, dictionary)
        refs.append(other)
    req, prop, reaction_map, hitboxes, _ = M.tekken8_maps([k], native, references=refs)
    k.attach_banks(motbin.read_bank(saved/'knk.anmbin'), motbin.read_bank(saved/'com.anmbin'))
    definition = SimpleNamespace(CODE='aki', NAME='Armor King', MOVESET={'name':'[ARMOR_KING]'})
    c = Graft(k, None, donor, archives.entry('data005',29), native[13], archives.entry('data004',0),
              (req, prop, reaction_map, hitboxes), M.ttt2_usage(native[:58]), 29, definition)
    c.aliases, c.newidx, c.alloc_ok, c.queue = list(donor.aliases), {}, 0, []
    c.voice = False
    c.tag_states = TS.TagStates(native[:58])
    target_bones = bone.read(archives.read_name('aki.bon'))
    c.hit_locations = weapons.points_of(target_bones['names'])
    c.slot_default = {f:collections.Counter(v[f] for v in donor.tables['reactions']).most_common(1)[0][0]
                      for f in REACTION_MOVE_FIELDS}
    bound = collections.Counter(h['reactions'] for h in donor.tables['hit_conditions']
                                if h['reactions'] is not None and any(q==467 for q,p in M.reqs(donor,h['req'])))
    c.bound_reaction = bound.most_common(1)[0][0]
    c.bound_req, c.after_bound_reaction = TS.bound_shape(donor.tables)
    ws3, ws31 = named_key(k,0x395d4590,10), named_key(k,0xba5dadfe,12)
    cd1, wall = named_key(k,0xa747eb75,9), named_key(k,0xd4fac58f,13)
    wall_hits = list(M.hitconds(k,k.tables['moves'][wall]['hit_condition']))
    cd_hits = list(M.hitconds(k,k.tables['moves'][cd1]['hit_condition']))
    if len(wall_hits)!=1 or len(cd_hits)!=1:
        raise InputError('Armor King source hit lists changed')
    victim = k.tables['reactions'][wall_hits[0][2]]['default']
    cd_reaction = cd_hits[0][2]
    air_moves = [k.tables['reactions'][cd_reaction][f] for f in ('default','left','right','back')]
    roots = list(dict.fromkeys([ws3,ws31,wall,victim]+air_moves))
    own, common = anmbin.Bank(saved/'knk.anmbin'), anmbin.Bank(saved/'com.anmbin')
    try:
        log('Converting selected animations onto native Armor King...')
        banks = anmbin.Banks(own,common,k.character_id)
        t8 = source.open_tekken8(None)
        c.animations = anim.Animations(banks,anim.Converter(anim.load_skeleton(t8.store,'knk'),target_bones))
        for i in roots:
            # These requested T8 clips must not silently reuse the older native clip.
            c.generic_anim.pop(k.anim_ref(i)[0],None)
        for i in roots:
            c.newidx[i]=len(c.T['moves'])
            c.T['moves'].append(c.conv_move(i))
        for i in roots:
            c.link_move(i,{})
        c.link_queued({})
        for forward in (False, True):
            native_moves.add_wall_throw(c,native_moves.wall_throw_template([native[17],native[30]],forward),
                                       c.newidx[wall],forward)
        # Extend WS3 down to the native left low-kick contact points.
        c.T['moves'][c.newidx[ws3]]['hitbox'] = donor.tables['moves'][c.dn['Ak_syalk00']]['hitbox']
        original_ws=c.dn['Ak_s_tlk00']
        native_moves.redirect_move_inputs(c,original_ws,c.newidx[ws3])
        native_moves.air_normal_reaction(c,c.dn['Ak_Akdslp'],c.conv_reaction(cd_reaction))
        c.link_queued({})
        native_moves.copy_air_reaction(c,c.newidx[ws31],c.dn['Ak_6rplp'])
        native_moves.add_breath_visual(c,c.newidx[wall],(115,127,142),1,5)
        c.handoffs8()
        for n,row in enumerate(c.T['moves'][len(donor.tables['moves']):],len(donor.tables['moves'])):
            combined=(29<<16)+n-28
            row['move_id_hi'],row['move_id_lo']=combined>>16 & 65535,combined & 65535
        if len(c.T['pushback_extras'])%2:
            c.T['pushback_extras'].append({'value':0})
        if c.stats.get("animations: none (the donor stance's)",0):
            raise InputError('a requested Armor King animation is missing')
        native_moves.validate_command_groups(c)
        mb,pk=c.write()
    finally:
        own.close()
        common.close()
    check=native_motbin.read(mb)
    if check.aliases!=donor.aliases or len(check.tables['moves'])!=len(c.T['moves']):
        raise InputError('native moveset serialization changed the native aliases or move count')
    rpx=importer._rpx(str(game))
    keys=[rpx.u32(0x105a51c4+29*12+i*4) for i in range(2)]
    for line in c.summary():log(line)
    return {keys[0]:pk,keys[1]:mb}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game',required=True,type=Path)
    parser.add_argument('--saved',required=True,type=Path)
    parser.add_argument('--out',required=True,type=Path,help='staging directory (contains keys/)')
    args=parser.parse_args()
    files=convert(args.game,args.saved,log=lambda s:print(s,flush=True))
    keys=args.out/'keys'
    keys.mkdir(parents=True,exist_ok=True)
    hashes={}
    for key,blob in files.items():
        path=keys/('%08x.bin'%key)
        temporary=path.with_suffix('.part')
        temporary.write_bytes(blob)
        temporary.replace(path)
        hashes[path.name]=hashlib.sha256(blob).hexdigest()
    (args.out/'patch.json').write_text(json.dumps({'code':'aki','revision':4,'files':hashes},indent=2)+'\n')
    print('Armor King patch built in '+str(args.out),flush=True)


if __name__=='__main__':main()
