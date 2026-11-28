//! Minimal SPIR-V reflection for slang passes: the uniform block (set 0), the push constant
//! block and the combined image samplers (set 0) with their names, member offsets and types.

use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Base {
    Float,
    Int,
    Uint,
    Other,
}

#[derive(Clone, Debug)]
pub struct Member {
    pub name: String,
    pub offset: u32,
    pub base: Base,
    /// scalar components (1 .. 4, 16 for mat4)
    pub components: u32,
}

#[derive(Clone, Debug, Default)]
pub struct Block {
    pub binding: u32,
    pub size: u32,
    pub members: Vec<Member>,
}

#[derive(Clone, Debug)]
pub struct Sampler {
    pub name: String,
    pub binding: u32,
}

#[derive(Clone, Debug, Default)]
pub struct Reflection {
    pub ubo: Option<Block>,
    pub push: Option<Block>,
    pub samplers: Vec<Sampler>,
    /// interface locations: vertex outputs, fragment inputs
    pub outputs: Vec<u32>,
    pub inputs: Vec<u32>,
}

#[derive(Clone, Debug)]
enum Type {
    Scalar(Base, u32),
    Vector(u32, u32),
    Matrix(u32, u32),
    Struct(Vec<u32>),
    Pointer(u32),
    Array(u32, u32),
    SampledImage,
    Other,
}

const OP_NAME: u32 = 5;
const OP_MEMBER_NAME: u32 = 6;
const OP_TYPE_INT: u32 = 21;
const OP_TYPE_FLOAT: u32 = 22;
const OP_TYPE_VECTOR: u32 = 23;
const OP_TYPE_MATRIX: u32 = 24;
const OP_TYPE_SAMPLED_IMAGE: u32 = 27;
const OP_TYPE_ARRAY: u32 = 28;
const OP_TYPE_STRUCT: u32 = 30;
const OP_TYPE_POINTER: u32 = 32;
const OP_CONSTANT: u32 = 43;
const OP_FUNCTION_CALL: u32 = 57;
const OP_VARIABLE: u32 = 59;
const OP_LOAD: u32 = 61;
const OP_COPY_MEMORY: u32 = 63;
const OP_ACCESS_CHAIN: u32 = 65;
const OP_IN_BOUNDS_ACCESS_CHAIN: u32 = 66;
const OP_PTR_ACCESS_CHAIN: u32 = 67;
const OP_DECORATE: u32 = 71;
const OP_MEMBER_DECORATE: u32 = 72;

const DEC_ARRAY_STRIDE: u32 = 6;
const DEC_LOCATION: u32 = 30;
const DEC_BINDING: u32 = 33;
const DEC_SET: u32 = 34;
const DEC_OFFSET: u32 = 35;

const SC_UNIFORM_CONSTANT: u32 = 0;
const SC_INPUT: u32 = 1;
const SC_OUTPUT: u32 = 3;
const SC_UNIFORM: u32 = 2;
const SC_PUSH_CONSTANT: u32 = 9;

fn string(words: &[u32]) -> String {
    let bytes: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).take_while(|&b| b != 0).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

struct Module {
    names: HashMap<u32, String>,
    member_names: HashMap<(u32, u32), String>,
    member_offsets: HashMap<(u32, u32), u32>,
    bindings: HashMap<u32, u32>,
    sets: HashMap<u32, u32>,
    locations: HashMap<u32, u32>,
    strides: HashMap<u32, u32>,
    types: HashMap<u32, Type>,
    constants: HashMap<u32, u32>,
    /// (variable id, pointer type, storage class)
    variables: Vec<(u32, u32, u32)>,
    /// ids used by loads, access chains, copies and calls (RetroArch reflects only used resources)
    used: HashSet<u32>,
    /// block members reached through access chains: (variable, member index)
    used_members: HashSet<(u32, u32)>,
}

impl Module {
    fn parse(spv: &[u32]) -> Result<Self, String> {
        if spv.len() < 5 || spv[0] != 0x0723_0203 {
            return Err("not a SPIR-V module".into());
        }
        let mut m = Module {
            names: HashMap::new(),
            member_names: HashMap::new(),
            member_offsets: HashMap::new(),
            bindings: HashMap::new(),
            sets: HashMap::new(),
            locations: HashMap::new(),
            strides: HashMap::new(),
            types: HashMap::new(),
            constants: HashMap::new(),
            variables: Vec::new(),
            used: HashSet::new(),
            used_members: HashSet::new(),
        };
        let mut i = 5;
        while i < spv.len() {
            let count = (spv[i] >> 16) as usize;
            let op = spv[i] & 0xffff;
            if count == 0 || i + count > spv.len() {
                return Err("truncated SPIR-V".into());
            }
            let a = &spv[i + 1..i + count];
            match op {
                OP_NAME if !a.is_empty() => {
                    m.names.insert(a[0], string(&a[1..]));
                }
                OP_MEMBER_NAME if a.len() >= 2 => {
                    m.member_names.insert((a[0], a[1]), string(&a[2..]));
                }
                OP_DECORATE if a.len() >= 3 => match a[1] {
                    DEC_BINDING => {
                        m.bindings.insert(a[0], a[2]);
                    }
                    DEC_SET => {
                        m.sets.insert(a[0], a[2]);
                    }
                    DEC_LOCATION => {
                        m.locations.insert(a[0], a[2]);
                    }
                    DEC_ARRAY_STRIDE => {
                        m.strides.insert(a[0], a[2]);
                    }
                    _ => {}
                },
                OP_MEMBER_DECORATE if a.len() >= 4 && a[2] == DEC_OFFSET => {
                    m.member_offsets.insert((a[0], a[1]), a[3]);
                }
                OP_TYPE_INT if a.len() >= 3 => {
                    let base = if a[2] != 0 { Base::Int } else { Base::Uint };
                    m.types.insert(a[0], Type::Scalar(base, a[1]));
                }
                OP_TYPE_FLOAT if a.len() >= 2 => {
                    m.types.insert(a[0], Type::Scalar(Base::Float, a[1]));
                }
                OP_TYPE_VECTOR if a.len() >= 3 => {
                    m.types.insert(a[0], Type::Vector(a[1], a[2]));
                }
                OP_TYPE_MATRIX if a.len() >= 3 => {
                    m.types.insert(a[0], Type::Matrix(a[1], a[2]));
                }
                OP_TYPE_STRUCT if !a.is_empty() => {
                    m.types.insert(a[0], Type::Struct(a[1..].to_vec()));
                }
                OP_TYPE_POINTER if a.len() >= 3 => {
                    m.types.insert(a[0], Type::Pointer(a[2]));
                }
                OP_TYPE_ARRAY if a.len() >= 3 => {
                    m.types.insert(a[0], Type::Array(a[1], a[2]));
                }
                OP_TYPE_SAMPLED_IMAGE if !a.is_empty() => {
                    m.types.insert(a[0], Type::SampledImage);
                }
                OP_CONSTANT if a.len() >= 3 => {
                    m.constants.insert(a[1], a[2]);
                }
                OP_VARIABLE if a.len() >= 3 => {
                    m.variables.push((a[1], a[0], a[2]));
                }
                OP_LOAD if a.len() >= 3 => {
                    m.used.insert(a[2]);
                }
                OP_COPY_MEMORY if a.len() >= 2 => {
                    m.used.insert(a[0]);
                    m.used.insert(a[1]);
                }
                OP_FUNCTION_CALL if a.len() >= 3 => {
                    m.used.extend(&a[3..]);
                }
                OP_ACCESS_CHAIN | OP_IN_BOUNDS_ACCESS_CHAIN | OP_PTR_ACCESS_CHAIN if a.len() >= 3 => {
                    // first index selects the block member (OpPtrAccessChain: element first)
                    let first = if op == OP_PTR_ACCESS_CHAIN { 4 } else { 3 };
                    match a.get(first).and_then(|c| m.constants.get(c)) {
                        Some(&member) => {
                            m.used_members.insert((a[2], member));
                        }
                        None => {
                            m.used.insert(a[2]);
                        }
                    }
                }
                _ => {
                    if (19..=39).contains(&op) && !a.is_empty() && !m.types.contains_key(&a[0]) {
                        // other type declarations (void, bool, image, sampler, function ...)
                        m.types.insert(a[0], Type::Other);
                    }
                }
            }
            i += count;
        }
        Ok(m)
    }

    /// (base type, scalar components, byte size)
    fn layout(&self, ty: u32) -> (Base, u32, u32) {
        match self.types.get(&ty) {
            Some(Type::Scalar(b, w)) => (*b, 1, w / 8),
            Some(Type::Vector(c, n)) => {
                let (b, _, s) = self.layout(*c);
                (b, *n, s * n)
            }
            Some(Type::Matrix(col, n)) => {
                let (b, comps, s) = self.layout(*col);
                // std140/std430 matrices: columns padded to vec4 (only vec4 columns matter here)
                let stride = if comps == 3 { s / 3 * 4 } else { s };
                (b, comps * n, stride * n)
            }
            Some(Type::Array(elem, len)) => {
                let n = self.constants.get(len).copied().unwrap_or(1);
                let stride = self.strides.get(&ty).copied().unwrap_or_else(|| self.layout(*elem).2);
                (Base::Other, 0, stride * n)
            }
            Some(Type::Struct(members)) => {
                let mut size = 0;
                for (i, m) in members.iter().enumerate() {
                    let off = self.member_offsets.get(&(ty, i as u32)).copied().unwrap_or(0);
                    size = size.max(off + self.layout(*m).2);
                }
                (Base::Other, 0, size)
            }
            _ => (Base::Other, 0, 0),
        }
    }

    /// the members of block variable `var` the shader reads (all of them when the whole
    /// block is loaded or passed on)
    fn block(&self, var: u32, ty: u32, binding: u32) -> Block {
        let Some(Type::Struct(members)) = self.types.get(&ty) else {
            return Block::default();
        };
        let mut block = Block {
            binding,
            size: self.layout(ty).2,
            members: Vec::new(),
        };
        let whole = self.used.contains(&var);
        for (i, &m) in members.iter().enumerate() {
            if !whole && !self.used_members.contains(&(var, i as u32)) {
                continue;
            }
            let key = (ty, i as u32);
            let (base, components, _) = self.layout(m);
            block.members.push(Member {
                name: self.member_names.get(&key).cloned().unwrap_or_default(),
                offset: self.member_offsets.get(&key).copied().unwrap_or(0),
                base,
                components,
            });
        }
        block
    }
}

fn merge(into: &mut Option<Block>, other: Block, what: &str) -> Result<(), String> {
    match into {
        None => *into = Some(other),
        Some(b) => {
            if b.binding != other.binding {
                return Err(format!("{what}: binding differs between vertex and fragment stage"));
            }
            b.size = b.size.max(other.size);
            for m in other.members {
                if !b.members.iter().any(|x| x.name == m.name) {
                    b.members.push(m);
                }
            }
        }
    }
    Ok(())
}

/// reflects one stage and merges it into `r`
pub fn reflect_stage(spv: &[u32], fragment: bool, r: &mut Reflection) -> Result<(), String> {
    let m = Module::parse(spv)?;
    for &(var, _, class) in &m.variables {
        if let Some(&loc) = m.locations.get(&var) {
            match (class, fragment) {
                (SC_OUTPUT, false) => r.outputs.push(loc),
                (SC_INPUT, true) => r.inputs.push(loc),
                _ => {}
            }
        }
    }
    for &(var, ptr, class) in &m.variables {
        let Some(Type::Pointer(pointee)) = m.types.get(&ptr) else {
            continue;
        };
        let pointee = *pointee;
        let set = m.sets.get(&var).copied().unwrap_or(0);
        let binding = m.bindings.get(&var).copied().unwrap_or(0);
        match class {
            SC_UNIFORM => {
                if set != 0 {
                    return Err("uniform buffer outside descriptor set 0".into());
                }
                merge(&mut r.ubo, m.block(var, pointee, binding), "uniform buffer")?;
            }
            SC_PUSH_CONSTANT => merge(&mut r.push, m.block(var, pointee, 0), "push constants")?,
            SC_UNIFORM_CONSTANT => {
                if !matches!(m.types.get(&pointee), Some(Type::SampledImage)) || !m.used.contains(&var) {
                    continue;
                }
                if set != 0 {
                    return Err("sampler outside descriptor set 0".into());
                }
                let name = m.names.get(&var).cloned().unwrap_or_default();
                if let Some(s) = r.samplers.iter().find(|s| s.binding == binding) {
                    if s.name != name {
                        return Err(format!("binding {binding} used by '{}' and '{name}'", s.name));
                    }
                } else {
                    r.samplers.push(Sampler { name, binding });
                }
            }
            _ => {}
        }
    }
    if let Some(ubo) = &r.ubo
        && r.samplers.iter().any(|s| s.binding == ubo.binding)
    {
        return Err(format!("binding {} used by the uniform buffer and a sampler", ubo.binding));
    }
    Ok(())
}
