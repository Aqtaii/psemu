#include "graphics/shader/recompiler/ShaderCFG.h"

#include <algorithm>
#include <fmt/format.h>
#include <iterator>
#include <map>
#include <set>
#include <stack>

namespace Libs::Graphics::ShaderRecompiler::CFG {

// PSEMU_CFG_HARITA=1: yapilandirici izleri. Sentetik blok yaratimi ve
// merge/continue atamalari. Amac, AYNI sentetik blogun hem bir dongunun
// continue/merge'i hem de bir secimin merge'i olarak atandigi ani
// suclustu yakalamak - SPIR-V'de bir blok yalnizca TEK bir yapinin
// merge'i olabilir.
bool CfgTraceEnabled() {
	static const bool on = [] {
		const char* env = std::getenv("PSEMU_CFG_HARITA");
		return env != nullptr && env[0] == '1';
	}();
	return on;
}

namespace {

using Decoder::Instruction;
using Decoder::Opcode;

struct SetpcTargetInfo {
	uint32_t              target        = 0;
	bool                  indirect      = false;
	uint32_t              pc_sgpr       = UINT32_MAX;
	uint32_t              selector_code = UINT32_MAX;
	uint32_t              table_load_pc = UINT32_MAX;
	std::vector<uint32_t> target_pcs;
	std::vector<uint32_t> selector_values;
	std::vector<uint32_t> selector_target_pcs;
};

void SetError(std::string* error, const std::string& message) {
	if (error != nullptr) {
		*error = message;
	}
}

void SetFailure(Graph* graph, FailureKind kind, uint32_t block_id, const std::string& message,
                std::string* error) {
	if (graph != nullptr) {
		graph->unsupported        = true;
		graph->failure_kind       = kind;
		graph->failure_block      = block_id;
		graph->unsupported_reason = message;
	}
	SetError(error, message);
}

uint32_t InstructionEndPc(const Instruction& inst) {
	return inst.pc + inst.word_count * 4u;
}

uint32_t ProgramEndPc(const Decoder::Program& program) {
	if (program.instructions.empty()) {
		return 0;
	}
	return InstructionEndPc(program.instructions.back());
}

bool IsUnconditionalBranch(Opcode opcode) {
	return opcode == Opcode::SBranch;
}

bool IsConditionalBranch(Opcode opcode) {
	switch (opcode) {
		case Opcode::SCbranchScc0:
		case Opcode::SCbranchScc1:
		case Opcode::SCbranchVccz:
		case Opcode::SCbranchVccnz:
		case Opcode::SCbranchExecz:
		case Opcode::SCbranchExecnz: return true;
		default: return false;
	}
}

bool IsBranch(Opcode opcode) {
	return IsUnconditionalBranch(opcode) || IsConditionalBranch(opcode);
}

BranchCondition ConditionForOpcode(Opcode opcode) {
	switch (opcode) {
		case Opcode::SBranch: return BranchCondition::Always;
		case Opcode::SCbranchScc0: return BranchCondition::SccZero;
		case Opcode::SCbranchScc1: return BranchCondition::SccNonZero;
		case Opcode::SCbranchVccz: return BranchCondition::VccZero;
		case Opcode::SCbranchVccnz: return BranchCondition::VccNonZero;
		case Opcode::SCbranchExecz: return BranchCondition::ExecZero;
		case Opcode::SCbranchExecnz: return BranchCondition::ExecNonZero;
		default: return BranchCondition::Unknown;
	}
}

bool IsRegister(const Decoder::Operand& operand, Decoder::OperandKind kind, uint32_t reg) {
	return operand.kind == kind && operand.reg == reg;
}

bool ScalarOperandCode(const Decoder::Operand& operand, uint32_t* code) {
	if (code == nullptr) {
		return false;
	}
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: *code = operand.reg; return true;
		case Decoder::OperandKind::VccLo: *code = 106u; return true;
		case Decoder::OperandKind::VccHi: *code = 107u; return true;
		case Decoder::OperandKind::M0: *code = 124u; return true;
		case Decoder::OperandKind::ExecLo: *code = 126u; return true;
		case Decoder::OperandKind::ExecHi: *code = 127u; return true;
		default: return false;
	}
}

bool IsScalarCode(const Decoder::Operand& operand, uint32_t code) {
	uint32_t operand_code = 0;
	return ScalarOperandCode(operand, &operand_code) && operand_code == code;
}

bool IsImmediate(const Decoder::Operand& operand, uint32_t* value) {
	if (operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::LiteralConstant ||
	    operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		if (value != nullptr) {
			*value = operand.value;
		}
		return true;
	}
	return false;
}

bool IsImmediateSigned(const Decoder::Operand& operand, int32_t* value) {
	if (operand.kind == Decoder::OperandKind::IntegerInlineConstant ||
	    operand.kind == Decoder::OperandKind::LiteralConstant) {
		if (value != nullptr) {
			*value = operand.signed_val;
		}
		return true;
	}
	if (operand.kind == Decoder::OperandKind::FloatInlineConstant) {
		if (value != nullptr) {
			*value = static_cast<int32_t>(operand.value);
		}
		return true;
	}
	return false;
}

bool OtherScalarSource(const Instruction& inst, uint32_t known_code,
                       const Decoder::Operand** other) {
	if (other == nullptr) {
		return false;
	}
	if (IsScalarCode(inst.src0, known_code)) {
		*other = &inst.src1;
		return true;
	}
	if (IsScalarCode(inst.src1, known_code)) {
		*other = &inst.src0;
		return true;
	}
	return false;
}

bool InstructionWritesScalarCode(const Instruction& inst, uint32_t code) {
	uint32_t dst_code = 0;
	if (!ScalarOperandCode(inst.dst, &dst_code)) {
		return false;
	}
	const uint32_t count = std::max<uint32_t>(1u, inst.data_dwords);
	return code >= dst_code && code < dst_code + count;
}

bool FindPreviousGetpc(const Decoder::Program& program, uint32_t before_index, uint32_t dst_code,
                       uint32_t* index) {
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (candidate.opcode == Opcode::SGetpcB64 && IsScalarCode(candidate.dst, dst_code)) {
			if (index != nullptr) {
				*index = candidate_index;
			}
			return true;
		}
		if (InstructionWritesScalarCode(candidate, dst_code)) {
			return false;
		}
	}
	return false;
}

bool ResolvePcRelativeBase(const Decoder::Program& program, uint32_t before_index,
                           uint32_t base_code, uint32_t* pc) {
	if (pc == nullptr) {
		return false;
	}
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (!InstructionWritesScalarCode(candidate, base_code)) {
			continue;
		}
		const bool adds =
		    candidate.opcode == Opcode::SAddU32 || candidate.opcode == Opcode::SAddI32;
		const bool subs =
		    candidate.opcode == Opcode::SSubU32 || candidate.opcode == Opcode::SSubI32;
		if (!adds && !subs) {
			return false;
		}

		const Decoder::Operand* offset      = nullptr;
		int32_t                 imm         = 0;
		uint32_t                getpc_index = 0;
		if (!OtherScalarSource(candidate, base_code, &offset) ||
		    !IsImmediateSigned(*offset, &imm) ||
		    !FindPreviousGetpc(program, candidate_index, base_code, &getpc_index)) {
			return false;
		}

		const auto base = InstructionEndPc(program.instructions[getpc_index]);
		*pc = adds ? base + static_cast<uint32_t>(imm) : base - static_cast<uint32_t>(imm);
		*pc &= ~3u;
		return true;
	}
	return false;
}

bool FindPreviousScalarLoadPair(const Decoder::Program& program, uint32_t before_index,
                                uint32_t dst_code, uint32_t* index) {
	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t candidate_index = i - 1u;
		const auto&    candidate       = program.instructions[candidate_index];
		if (InstructionWritesScalarCode(candidate, dst_code)) {
			if (candidate.opcode == Opcode::SLoadDwordx2 && IsScalarCode(candidate.dst, dst_code)) {
				if (index != nullptr) {
					*index = candidate_index;
				}
				return true;
			}
			return false;
		}
	}
	return false;
}

bool ResolveJumpTableEntryCount(const Decoder::Program& program, uint32_t before_index,
                                const Decoder::Operand& byte_offset_operand,
                                uint32_t*               entry_count) {
	if (entry_count == nullptr) {
		return false;
	}
	uint32_t byte_offset_code = 0;
	if (!ScalarOperandCode(byte_offset_operand, &byte_offset_code)) {
		return false;
	}

	for (uint32_t i = before_index; i > 0; i--) {
		const uint32_t shift_index = i - 1u;
		const auto&    shift       = program.instructions[shift_index];
		if (!InstructionWritesScalarCode(shift, byte_offset_code)) {
			continue;
		}
		if (shift.opcode != Opcode::SLshlB32 || !IsScalarCode(shift.dst, byte_offset_code)) {
			return false;
		}

		const Decoder::Operand* shift_amount_operand = nullptr;
		uint32_t                shift_amount         = 0;
		if (!OtherScalarSource(shift, byte_offset_code, &shift_amount_operand) ||
		    !IsImmediate(*shift_amount_operand, &shift_amount) || shift_amount != 3u) {
			return false;
		}

		uint32_t index_code = byte_offset_code;

		for (uint32_t j = shift_index; j > 0; j--) {
			const uint32_t clamp_index = j - 1u;
			const auto&    clamp       = program.instructions[clamp_index];
			if (!InstructionWritesScalarCode(clamp, index_code)) {
				continue;
			}
			if (clamp.opcode != Opcode::SMinU32 || !IsScalarCode(clamp.dst, index_code)) {
				return false;
			}

			const Decoder::Operand* clamp_other = nullptr;
			uint32_t                max_index   = 0;
			if (!OtherScalarSource(clamp, index_code, &clamp_other) ||
			    !IsImmediate(*clamp_other, &max_index)) {
				return false;
			}
			*entry_count = max_index + 1u;
			return *entry_count != 0;
		}
		return false;
	}
	return false;
}

bool AddUniqueTargetPc(std::vector<uint32_t>* targets, uint32_t target) {
	if (targets == nullptr) {
		return false;
	}
	if (std::find(targets->begin(), targets->end(), target) == targets->end()) {
		targets->push_back(target);
	}
	return true;
}

bool ScalarCodeWrittenInRange(const Decoder::Program& program, uint32_t begin_index,
                              uint32_t end_index, uint32_t code) {
	const auto end =
	    std::min<uint32_t>(end_index, static_cast<uint32_t>(program.instructions.size()));
	for (uint32_t i = begin_index; i < end; i++) {
		if (InstructionWritesScalarCode(program.instructions[i], code)) {
			return true;
		}
	}
	return false;
}

bool ResolveSetpcJumpTable(const Decoder::Program& program, uint32_t setpc_index,
                           SetpcTargetInfo* info) {
	if (info == nullptr || setpc_index < 4u || setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc  = program.instructions[setpc_index];
	uint32_t    pc_reg = 0;
	if (setpc.opcode != Opcode::SSetpcB64 || !ScalarOperandCode(setpc.src0, &pc_reg) ||
	    setpc.src0.kind != Decoder::OperandKind::Sgpr) {
		return false;
	}

	const auto& getpc = program.instructions[setpc_index - 3u];
	const auto& add   = program.instructions[setpc_index - 2u];
	const auto& addc  = program.instructions[setpc_index - 1u];
	if (getpc.opcode != Opcode::SGetpcB64 || !IsScalarCode(getpc.dst, pc_reg) ||
	    add.opcode != Opcode::SAddU32 || !IsScalarCode(add.dst, pc_reg) ||
	    addc.opcode != Opcode::SAddcU32 || !IsScalarCode(addc.dst, pc_reg + 1u)) {
		return false;
	}

	const Decoder::Operand* offset_low_operand  = nullptr;
	const Decoder::Operand* offset_high_operand = nullptr;
	uint32_t                offset_low_code     = 0;
	uint32_t                offset_high_code    = 0;
	if (!OtherScalarSource(add, pc_reg, &offset_low_operand) ||
	    !OtherScalarSource(addc, pc_reg + 1u, &offset_high_operand) ||
	    !ScalarOperandCode(*offset_low_operand, &offset_low_code) ||
	    !ScalarOperandCode(*offset_high_operand, &offset_high_code) ||
	    offset_high_code != offset_low_code + 1u) {
		return false;
	}

	uint32_t load_index = 0;
	if (!FindPreviousScalarLoadPair(program, setpc_index - 3u, offset_low_code, &load_index)) {
		return false;
	}
	const auto& load            = program.instructions[load_index];
	uint32_t    table_base_code = 0;
	if (!ScalarOperandCode(load.src0, &table_base_code)) {
		return false;
	}

	uint32_t table_pc = 0;
	if (!ResolvePcRelativeBase(program, load_index, table_base_code, &table_pc)) {
		return false;
	}

	uint32_t entry_count = 0;
	if (!ResolveJumpTableEntryCount(program, load_index, load.src1, &entry_count)) {
		return false;
	}
	uint32_t selector_code = UINT32_MAX;
	if (!ScalarOperandCode(load.src1, &selector_code) ||
	    ScalarCodeWrittenInRange(program, load_index + 1u, setpc_index, selector_code)) {
		return false;
	}

	const uint32_t target_base = InstructionEndPc(getpc);
	const uint32_t table_word  = table_pc / 4u;
	if ((table_pc & 3u) != 0 || table_word + entry_count * 2u > program.code.size()) {
		return false;
	}

	std::vector<uint32_t> targets;
	std::vector<uint32_t> selector_values;
	std::vector<uint32_t> selector_target_pcs;
	for (uint32_t i = 0; i < entry_count; i++) {
		const uint32_t low  = program.code[table_word + i * 2u];
		const uint32_t high = program.code[table_word + i * 2u + 1u];
		if (high != 0u && high != UINT32_MAX) {
			return false;
		}
		const auto target_pc = (target_base + low) & ~3u;
		AddUniqueTargetPc(&targets, target_pc);
		selector_values.push_back(i * 8u);
		selector_target_pcs.push_back(target_pc);
	}
	if (targets.empty()) {
		return false;
	}

	info->indirect            = true;
	info->pc_sgpr             = pc_reg;
	info->selector_code       = selector_code;
	info->table_load_pc       = load.pc;
	info->target_pcs          = std::move(targets);
	info->selector_values     = std::move(selector_values);
	info->selector_target_pcs = std::move(selector_target_pcs);
	return true;
}

bool ResolveSetpcTarget(const Decoder::Program& program, uint32_t setpc_index, uint32_t* target) {
	if (target == nullptr || setpc_index >= program.instructions.size()) {
		return false;
	}

	const auto& setpc = program.instructions[setpc_index];
	if (setpc.opcode != Opcode::SSetpcB64 || setpc.src0.kind != Decoder::OperandKind::Sgpr) {
		return false;
	}

	const auto pc_reg = setpc.src0.reg;
	if (setpc_index >= 2u) {
		const auto& arith = program.instructions[setpc_index - 1u];
		const auto& getpc = program.instructions[setpc_index - 2u];
		if (getpc.opcode == Opcode::SGetpcB64 && getpc.dst.kind == Decoder::OperandKind::Sgpr &&
		    getpc.dst.reg == pc_reg && arith.dst.kind == Decoder::OperandKind::Sgpr &&
		    arith.dst.reg == pc_reg) {
			uint32_t imm  = 0;
			bool     adds = arith.opcode == Opcode::SAddU32 || arith.opcode == Opcode::SAddI32;
			bool     subs = arith.opcode == Opcode::SSubU32 || arith.opcode == Opcode::SSubI32;
			if ((adds || subs) &&
			    (IsRegister(arith.src0, Decoder::OperandKind::Sgpr, pc_reg) ||
			     IsRegister(arith.src1, Decoder::OperandKind::Sgpr, pc_reg)) &&
			    (IsImmediate(arith.src0, &imm) || IsImmediate(arith.src1, &imm))) {
				const auto base = InstructionEndPc(getpc);
				*target         = adds ? base + imm : base - imm;
				*target &= ~3u;
				return true;
			}
		}
	}

	if (setpc_index >= 1u) {
		const auto& getpc = program.instructions[setpc_index - 1u];
		if (getpc.opcode == Opcode::SGetpcB64 && getpc.dst.kind == Decoder::OperandKind::Sgpr &&
		    getpc.dst.reg == pc_reg) {
			*target = InstructionEndPc(getpc);
			return true;
		}
	}

	return false;
}

bool ResolveSetpcTargets(const Decoder::Program& program, uint32_t setpc_index,
                         SetpcTargetInfo* info) {
	if (info == nullptr) {
		return false;
	}
	*info = {};
	if (ResolveSetpcJumpTable(program, setpc_index, info)) {
		return true;
	}
	uint32_t target = 0;
	if (ResolveSetpcTarget(program, setpc_index, &target)) {
		info->target = target;
		return true;
	}
	return false;
}

bool IsValidTarget(uint32_t target, const std::set<uint32_t>& instruction_pcs, uint32_t first_pc,
                   uint32_t end_pc) {
	return target == end_pc || (target >= first_pc && instruction_pcs.contains(target));
}

void AddUnique(std::vector<uint32_t>* values, uint32_t value) {
	if (std::find(values->begin(), values->end(), value) == values->end()) {
		values->push_back(value);
	}
}

std::vector<uint32_t> AllBlockIds(uint32_t count) {
	std::vector<uint32_t> ids;
	ids.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		ids.push_back(i);
	}
	return ids;
}

std::vector<uint32_t> IntersectSorted(const std::vector<uint32_t>& a,
                                      const std::vector<uint32_t>& b) {
	std::vector<uint32_t> ret;
	ret.reserve(std::min(a.size(), b.size()));
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(ret));
	return ret;
}

void SortUnique(std::vector<uint32_t>* values) {
	std::sort(values->begin(), values->end());
	values->erase(std::unique(values->begin(), values->end()), values->end());
}

bool Contains(const std::vector<uint32_t>& values, uint32_t value) {
	return std::find(values.begin(), values.end(), value) != values.end();
}

bool ReplaceValue(std::vector<uint32_t>* values, uint32_t old_value, uint32_t new_value) {
	bool changed = false;
	for (auto& value: *values) {
		if (value == old_value) {
			value   = new_value;
			changed = true;
		}
	}
	if (changed) {
		SortUnique(values);
	}
	return changed;
}

bool RemoveValue(std::vector<uint32_t>* values, uint32_t value) {
	const auto old_size = values->size();
	values->erase(std::remove(values->begin(), values->end(), value), values->end());
	return values->size() != old_size;
}

bool ReplaceTerminatorTarget(Terminator* terminator, uint32_t old_value, uint32_t new_value) {
	bool changed = false;
	if (terminator->true_block == old_value) {
		terminator->true_block = new_value;
		changed                = true;
	}
	if (terminator->false_block == old_value) {
		terminator->false_block = new_value;
		changed                 = true;
	}
	return changed;
}

uint32_t RemapId(uint32_t id, const std::vector<uint32_t>& id_map) {
	return id != UINT32_MAX && id < id_map.size() ? id_map[id] : id;
}

void RemapIds(std::vector<uint32_t>* values, const std::vector<uint32_t>& id_map) {
	for (auto& value: *values) {
		value = RemapId(value, id_map);
	}
	SortUnique(values);
}

void RebuildPredecessors(Graph* graph) {
	for (auto& block: graph->blocks) {
		block.predecessors.clear();
		SortUnique(&block.successors);
	}
	for (const auto& block: graph->blocks) {
		for (auto succ: block.successors) {
			if (succ < graph->blocks.size()) {
				AddUnique(&graph->blocks[succ].predecessors, block.id);
			}
		}
	}
	for (auto& block: graph->blocks) {
		SortUnique(&block.predecessors);
	}
}

void ComputeDominators(Graph* graph) {
	const auto count = static_cast<uint32_t>(graph->blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph->blocks) {
		block.dominators =
		    (block.id == graph->entry_block ? std::vector<uint32_t> {block.id} : all);
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& block: graph->blocks) {
			if (block.id == graph->entry_block) {
				continue;
			}
			std::vector<uint32_t> next;
			if (block.predecessors.empty()) {
				next = {block.id};
			} else {
				next = graph->blocks[block.predecessors.front()].dominators;
				for (uint32_t i = 1; i < block.predecessors.size(); i++) {
					next = IntersectSorted(next, graph->blocks[block.predecessors[i]].dominators);
				}
				AddUnique(&next, block.id);
				SortUnique(&next);
			}
			if (next != block.dominators) {
				block.dominators = std::move(next);
				changed          = true;
			}
		}
	}
}

void ComputePostDominators(Graph* graph) {
	const auto count = static_cast<uint32_t>(graph->blocks.size());
	const auto all   = AllBlockIds(count);

	for (auto& block: graph->blocks) {
		block.post_dominators = block.successors.empty() ? std::vector<uint32_t> {block.id} : all;
	}

	bool changed = true;
	while (changed) {
		changed = false;
		for (auto& block: graph->blocks) {
			std::vector<uint32_t> next;
			if (block.successors.empty()) {
				next = {block.id};
			} else {
				next = graph->blocks[block.successors.front()].post_dominators;
				for (uint32_t i = 1; i < block.successors.size(); i++) {
					next =
					    IntersectSorted(next, graph->blocks[block.successors[i]].post_dominators);
				}
				AddUnique(&next, block.id);
				SortUnique(&next);
			}
			if (next != block.post_dominators) {
				block.post_dominators = std::move(next);
				changed               = true;
			}
		}
	}
}

void ComputeBackEdges(Graph* graph) {
	graph->back_edges.clear();
	for (const auto& block: graph->blocks) {
		for (auto succ: block.successors) {
			if (graph->Dominates(succ, block.id)) {
				graph->back_edges.push_back({block.id, succ, true});
			}
		}
	}
}

std::vector<uint32_t> NaturalLoopBody(const Graph& graph, uint32_t header, uint32_t latch,
                                      bool* natural) {
	std::vector<uint32_t> body;
	std::vector<uint32_t> stack;
	body.reserve(graph.blocks.size());
	stack.reserve(graph.blocks.size());
	AddUnique(&body, header);
	AddUnique(&body, latch);
	if (latch != header) {
		stack.push_back(latch);
	}
	if (natural != nullptr) {
		*natural = true;
	}

	while (!stack.empty()) {
		const auto block_id = stack.back();
		stack.pop_back();
		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr) {
			continue;
		}
		for (auto pred: block->predecessors) {
			if (!graph.Dominates(header, pred) && natural != nullptr) {
				*natural = false;
			}
			if (!Contains(body, pred)) {
				body.push_back(pred);
				if (pred != header) {
					stack.push_back(pred);
				}
			}
		}
	}
	SortUnique(&body);
	return body;
}

void ComputeNaturalLoops(Graph* graph) {
	graph->natural_loops.clear();
	for (auto& edge: graph->back_edges) {
		bool natural = true;
		auto body    = NaturalLoopBody(*graph, edge.to, edge.from, &natural);
		edge.natural = natural;

		NaturalLoop loop;
		loop.header         = edge.to;
		loop.latch          = edge.from;
		loop.continue_block = edge.from;
		loop.body_blocks    = body;

		for (auto block_id: body) {
			const auto* block = graph->FindBlock(block_id);
			if (block == nullptr) {
				continue;
			}
			for (auto succ: block->successors) {
				if (!Contains(body, succ)) {
					AddUnique(&loop.exit_blocks, succ);
				}
			}
		}
		SortUnique(&loop.exit_blocks);

		if (!loop.exit_blocks.empty()) {
			uint32_t merge = loop.exit_blocks.front();
			for (uint32_t i = 1; i < loop.exit_blocks.size(); i++) {
				merge = graph->FindNearestCommonPostDominator(merge, loop.exit_blocks[i]);
			}
			loop.merge = merge;
		}
		graph->natural_loops.push_back(std::move(loop));
	}
}

struct TarjanState {
	const Graph*                            graph      = nullptr;
	uint32_t                                next_index = 0;
	std::vector<uint32_t>                   index;
	std::vector<uint32_t>                   lowlink;
	std::vector<bool>                       on_stack;
	std::vector<uint32_t>                   stack;
	std::vector<StronglyConnectedComponent> components;
};

void TarjanVisit(TarjanState* state, uint32_t block_id) {
	state->index[block_id]   = state->next_index;
	state->lowlink[block_id] = state->next_index;
	state->next_index++;
	state->stack.push_back(block_id);
	state->on_stack[block_id] = true;

	const auto* block = state->graph->FindBlock(block_id);
	if (block != nullptr) {
		for (auto succ: block->successors) {
			if (state->index[succ] == UINT32_MAX) {
				TarjanVisit(state, succ);
				state->lowlink[block_id] = std::min(state->lowlink[block_id], state->lowlink[succ]);
			} else if (state->on_stack[succ]) {
				state->lowlink[block_id] = std::min(state->lowlink[block_id], state->index[succ]);
			}
		}
	}

	if (state->lowlink[block_id] != state->index[block_id]) {
		return;
	}

	StronglyConnectedComponent component;
	for (;;) {
		const auto member = state->stack.back();
		state->stack.pop_back();
		state->on_stack[member] = false;
		component.blocks.push_back(member);
		if (member == block_id) {
			break;
		}
	}
	SortUnique(&component.blocks);

	bool cyclic = component.blocks.size() > 1u;
	for (auto member: component.blocks) {
		const auto* member_block = state->graph->FindBlock(member);
		if (member_block != nullptr && Contains(member_block->successors, member)) {
			cyclic = true;
		}
		if (member_block != nullptr) {
			for (auto pred: member_block->predecessors) {
				if (!Contains(component.blocks, pred)) {
					AddUnique(&component.entry_blocks, member);
				}
			}
		}
	}
	SortUnique(&component.entry_blocks);
	component.irreducible = cyclic && component.entry_blocks.size() > 1u;
	state->components.push_back(std::move(component));
}

void ComputeComponents(Graph* graph) {
	TarjanState state;
	state.graph = graph;
	state.index.assign(graph->blocks.size(), UINT32_MAX);
	state.lowlink.assign(graph->blocks.size(), UINT32_MAX);
	state.on_stack.assign(graph->blocks.size(), false);
	state.stack.reserve(graph->blocks.size());
	state.components.reserve(graph->blocks.size());

	for (const auto& block: graph->blocks) {
		if (state.index[block.id] == UINT32_MAX) {
			TarjanVisit(&state, block.id);
		}
	}

	graph->components  = std::move(state.components);
	graph->irreducible = false;
	for (const auto& component: graph->components) {
		if (component.irreducible) {
			graph->irreducible   = true;
			graph->failure_kind  = FailureKind::IrreducibleControlFlow;
			graph->failure_block = component.entry_blocks.empty() ? component.blocks.front()
			                                                      : component.entry_blocks.front();
			graph->unsupported_reason = "irreducible CFG: cyclic component has multiple entries";
			break;
		}
	}
}

void RecomputeAnalyses(Graph* graph) {
	ComputeDominators(graph);
	ComputePostDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);
}

uint32_t MoveBlockBefore(Graph* graph, uint32_t block_id, uint32_t before_id) {
	if (block_id == before_id || block_id >= graph->blocks.size() ||
	    before_id >= graph->blocks.size()) {
		return block_id;
	}

	const auto              block_pos  = block_id;
	const auto              before_pos = before_id;
	std::vector<BasicBlock> old_blocks = std::move(graph->blocks);
	std::vector<BasicBlock> new_blocks;
	new_blocks.reserve(old_blocks.size());

	for (uint32_t i = 0; i < old_blocks.size(); i++) {
		if (i == before_pos) {
			new_blocks.push_back(std::move(old_blocks[block_pos]));
		}
		if (i != block_pos) {
			new_blocks.push_back(std::move(old_blocks[i]));
		}
	}

	std::vector<uint32_t> id_map(new_blocks.size(), UINT32_MAX);
	for (uint32_t i = 0; i < new_blocks.size(); i++) {
		id_map[new_blocks[i].id] = i;
	}

	// ROSETTA TASI: bu pas TUM blok kimliklerini yeniden numaralandiriyor ve
	// birden fazla kez calisabiliyor. Yapilandirma anindaki loglar ile yayim
	// anindaki kimlikler bu yuzden AYNI SEY DEGIL - gece boyunca ucu capraz
	// okundu. Yalnizca DEGISEN girdileri bas (degismeyenler gurultu).
	if (CfgTraceEnabled()) {
		std::string changes;
		for (uint32_t i = 0; i < id_map.size(); i++) {
			if (id_map[i] != UINT32_MAX && id_map[i] != i) {
				changes += fmt::format(" {}->{}", i, id_map[i]);
			}
		}
		std::printf("[REMAP] blok=%u once=%u degisen=%s\n", block_pos, before_pos,
		            changes.empty() ? " (yok)" : changes.c_str());
		std::fflush(stdout);
	}

	graph->blocks      = std::move(new_blocks);
	graph->entry_block = RemapId(graph->entry_block, id_map);
	for (auto& block: graph->blocks) {
		block.id = RemapId(block.id, id_map);
		RemapIds(&block.predecessors, id_map);
		RemapIds(&block.successors, id_map);
		RemapIds(&block.dominators, id_map);
		RemapIds(&block.post_dominators, id_map);
		block.terminator.true_block     = RemapId(block.terminator.true_block, id_map);
		block.terminator.false_block    = RemapId(block.terminator.false_block, id_map);
		block.terminator.merge_block    = RemapId(block.terminator.merge_block, id_map);
		block.terminator.continue_block = RemapId(block.terminator.continue_block, id_map);
	}

	return RemapId(block_id, id_map);
}

std::vector<uint32_t> DominatedBlocks(const Graph& graph, uint32_t header,
                                      uint32_t stop_block = UINT32_MAX) {
	std::vector<uint32_t> blocks;
	std::vector<uint32_t> stack = {header};
	blocks.reserve(graph.blocks.size());
	stack.reserve(graph.blocks.size());

	while (!stack.empty()) {
		const auto block_id = stack.back();
		stack.pop_back();
		if (block_id == stop_block || Contains(blocks, block_id) ||
		    !graph.Dominates(header, block_id)) {
			continue;
		}

		const auto* block = graph.FindBlock(block_id);
		if (block == nullptr) {
			continue;
		}

		AddUnique(&blocks, block_id);
		for (auto succ: block->successors) {
			if (succ != stop_block && graph.Dominates(header, succ)) {
				stack.push_back(succ);
			}
		}
	}

	SortUnique(&blocks);
	return blocks;
}

uint32_t AppendSyntheticMergeBlock(Graph* graph, uint32_t old_merge) {
	const auto* merge = graph->FindBlock(old_merge);

	BasicBlock block;
	block.id                    = static_cast<uint32_t>(graph->blocks.size());
	block.start_pc              = merge != nullptr ? merge->start_pc : 0u;
	block.end_pc                = block.start_pc;
	block.inst_begin            = merge != nullptr ? merge->inst_begin : 0u;
	block.inst_end              = block.inst_begin;
	block.successors            = {old_merge};
	block.terminator.kind       = TerminatorKind::Branch;
	block.terminator.condition  = BranchCondition::Always;
	block.terminator.true_block = old_merge;
	const auto new_id           = block.id;
	graph->blocks.push_back(std::move(block));
	if (CfgTraceEnabled()) {
		std::printf("[SENTETIK-YARAT] id=%u -> hedef=%u\n", new_id, old_merge);
		std::fflush(stdout);
	}
	return graph->blocks.back().id;
}

// HEDEFSIZ SURUM: blogun kendisi bos bir yonlendirici mi? Karar koruma
// kalkani icin gerekli - orada merge'un NEREYE yonlendirdigini bilmiyoruz,
// yalnizca "bu bir bolme pasinin urettigi sentetik blok mu" diye soruyoruz.
bool IsSyntheticForwarderBlock(const Graph& graph, uint32_t block_id) {
	const auto* block = graph.FindBlock(block_id);
	return block != nullptr && block->inst_begin == block->inst_end &&
	       block->successors.size() == 1u &&
	       block->terminator.kind == TerminatorKind::Branch &&
	       block->terminator.condition == BranchCondition::Always &&
	       block->terminator.true_block == block->successors.front();
}

bool IsSyntheticMergeForwarder(const Graph& graph, uint32_t block_id, uint32_t merge) {
	const auto* block = graph.FindBlock(block_id);
	return block != nullptr && block->inst_begin == block->inst_end &&
	       block->successors.size() == 1u && block->successors.front() == merge &&
	       block->terminator.kind == TerminatorKind::Branch &&
	       block->terminator.condition == BranchCondition::Always &&
	       block->terminator.true_block == merge;
}

bool IsInsideLoopConstruct(const Graph& graph, const NaturalLoop& loop, uint32_t block_id) {
	return block_id != UINT32_MAX && block_id != loop.merge && block_id != loop.continue_block &&
	       graph.Dominates(loop.header, block_id) &&
	       (loop.merge == UINT32_MAX || !graph.Dominates(loop.merge, block_id));
}

bool SelectionMergeLeavesContainingLoop(const Graph& graph, uint32_t header, uint32_t merge) {
	for (const auto& loop: graph.natural_loops) {
		if (IsInsideLoopConstruct(graph, loop, header) &&
		    !IsInsideLoopConstruct(graph, loop, merge)) {
			return true;
		}
	}
	return false;
}

std::string VectorToString(const std::vector<uint32_t>& values);

bool SplitSharedMergeBlock(Graph* graph, uint32_t merge,
                           const std::vector<uint32_t>& construct_blocks,
                           bool                         force_split = false) {
	if (graph == nullptr || merge == UINT32_MAX || merge >= graph->blocks.size() ||
	    Contains(construct_blocks, merge)) {
		return false;
	}

	const auto* merge_block = graph->FindBlock(merge);
	if (merge_block == nullptr) {
		return false;
	}

	std::vector<uint32_t> construct_predecessors;
	std::vector<uint32_t> external_predecessors;
	for (auto pred: merge_block->predecessors) {
		if (Contains(construct_blocks, pred)) {
			AddUnique(&construct_predecessors, pred);
		} else {
			AddUnique(&external_predecessors, pred);
		}
	}

	// TANI: bolme hangi on kosulda duruyor? Cakisan merge'leri onarmak icin
	// sart. Olculdu: gercek blokaj "tum_onculler_sentetik_yonlendirici" -
	// bolunecek tek oncul zaten onceki bir bolmenin urettigi yonlendirici.
	auto decline = [&](const char* reason) {
		static std::atomic<uint32_t> s_n {0};
		// Kapak YUKSEK: ilk olcumde 64 kaydin 64'u de ayni nedendi, gercek
		// blokaj kesilmis logun arkasinda kalmisti.
		if (s_n.fetch_add(1) < 100000) {
			std::printf("[CFG-BOLME-RED] merge=%u neden=%s | construct_onculleri=[%s] "
			            "dis_onculler=[%s] zorla=%d\n",
			            merge, reason, VectorToString(construct_predecessors).c_str(),
			            VectorToString(external_predecessors).c_str(), force_split ? 1 : 0);
			std::fflush(stdout);
		}
		return false;
	};

	if (construct_predecessors.empty()) {
		return decline("construct_onculu_yok");
	}
	if (!force_split && external_predecessors.empty()) {
		return decline("dis_oncul_yok");
	}
	std::vector<uint32_t> predecessors_to_split;
	for (auto pred: construct_predecessors) {
		if (!IsSyntheticMergeForwarder(*graph, pred, merge)) {
			AddUnique(&predecessors_to_split, pred);
		}
	}
	if (predecessors_to_split.empty()) {
		return decline("tum_onculler_sentetik_yonlendirici");
	}

	const auto synthetic_merge = AppendSyntheticMergeBlock(graph, merge);
	auto*      synthetic_block = graph->FindBlock(synthetic_merge);
	if (synthetic_block != nullptr) {
		synthetic_block->predecessors = predecessors_to_split;
		SortUnique(&synthetic_block->predecessors);
	}

	for (auto pred: predecessors_to_split) {
		auto* block = graph->FindBlock(pred);
		if (block == nullptr) {
			continue;
		}
		ReplaceValue(&block->successors, merge, synthetic_merge);
		ReplaceTerminatorTarget(&block->terminator, merge, synthetic_merge);
	}

	auto* old_merge = graph->FindBlock(merge);
	if (old_merge != nullptr) {
		for (auto pred: predecessors_to_split) {
			RemoveValue(&old_merge->predecessors, pred);
		}
		AddUnique(&old_merge->predecessors, synthetic_merge);
		SortUnique(&old_merge->predecessors);
	}

	MoveBlockBefore(graph, synthetic_merge, merge);
	return true;
}

bool SplitOneLoopMerge(Graph* graph) {
	const auto& loops = graph->natural_loops;
	for (const auto& loop: loops) {
		if (SplitSharedMergeBlock(graph, loop.merge, loop.body_blocks)) {
			return true;
		}
	}
	return false;
}

bool SplitOneSelectionMerge(Graph* graph) {
	std::vector<uint32_t> loop_headers;
	loop_headers.reserve(graph->natural_loops.size());
	for (const auto& loop: graph->natural_loops) {
		AddUnique(&loop_headers, loop.header);
	}

	const auto original_block_count = static_cast<uint32_t>(graph->blocks.size());
	for (uint32_t block_id = 0; block_id < original_block_count; block_id++) {
		const auto* block = graph->FindBlock(block_id);
		if (block == nullptr || block->terminator.kind != TerminatorKind::ConditionalBranch ||
		    Contains(loop_headers, block_id)) {
			continue;
		}

		const auto merge = graph->FindNearestCommonPostDominator(block->terminator.true_block,
		                                                         block->terminator.false_block);
		const auto construct_blocks = DominatedBlocks(*graph, block_id, merge);
		const auto force_split      = SelectionMergeLeavesContainingLoop(*graph, block_id, merge);
		if (SplitSharedMergeBlock(graph, merge, construct_blocks, force_split)) {
			return true;
		}
	}
	return false;
}

// Bir basligin GERCEK CIKIS blogu: domine ettigi kumeden disariya giden ilk
// kenarin hedefi. NCPD bunu asabiliyor - olculdu: header 11 icin NCPD 20
// donuyor ama gercek cikis 17 -> 18.
//
// Birden fazla farkli cikis varsa yapi zaten tek merge'e oturmuyor demektir;
// o durumda UINT32_MAX donup karari cagirana birakiyoruz.
uint32_t FindTrueExitBlock(const Graph& graph, uint32_t header,
                           const std::vector<uint32_t>& construct) {
	uint32_t exit_block = UINT32_MAX;
	for (auto id: construct) {
		const auto* block = graph.FindBlock(id);
		if (block == nullptr) {
			continue;
		}
		for (auto succ: block->successors) {
			if (Contains(construct, succ) || succ == header) {
				continue;
			}
			if (exit_block == UINT32_MAX) {
				exit_block = succ;
			} else if (exit_block != succ) {
				return UINT32_MAX;  // birden fazla cikis
			}
		}
	}
	return exit_block;
}

// ON TARAMA: hangi merge blogunu hangi basliklar talep ediyor?
// Su an YALNIZCA LOG - grafi degistirmiyor. Amac, cakisan basliklari ic-ice
// gecme sirasina koyup her biri icin gercek cikis blogunu programatik olarak
// yakaladigimizi dogrulamak. Elle yaptigimiz analizde header 11'in gercek
// cikisinin 18 oldugunu bulmustuk; bunu kod da bulmali.
void LogMergeClaimMap(const Graph& graph) {
	std::vector<uint32_t> loop_headers;
	for (const auto& loop: graph.natural_loops) {
		AddUnique(&loop_headers, loop.header);
	}

	std::map<uint32_t, std::vector<uint32_t>>            merge_claims;
	std::map<uint32_t, std::vector<uint32_t>>            constructs;
	for (const auto& block: graph.blocks) {
		if (block.terminator.kind != TerminatorKind::ConditionalBranch ||
		    Contains(loop_headers, block.id)) {
			continue;
		}
		const auto merge = graph.FindNearestCommonPostDominator(block.terminator.true_block,
		                                                        block.terminator.false_block);
		if (merge == UINT32_MAX) {
			continue;
		}
		merge_claims[merge].push_back(block.id);
		constructs[block.id] = DominatedBlocks(graph, block.id, merge);
	}

	for (const auto& [merge, headers]: merge_claims) {
		if (headers.size() < 2) {
			continue;
		}
		// Ic-ice sirasi: construct'i KUCUK olan en ictedir.
		auto sorted = headers;
		std::sort(sorted.begin(), sorted.end(), [&](uint32_t a, uint32_t b) {
			return constructs[a].size() < constructs[b].size();
		});
		std::printf("[CFG-HARITA] merge=%u talep_eden_baslik_sayisi=%zu (icten disa)\n", merge,
		            sorted.size());
		for (auto header: sorted) {
			const auto& construct  = constructs[header];
			const auto  true_exit  = FindTrueExitBlock(graph, header, construct);
			std::printf("[CFG-HARITA]   header=%u construct_boyu=%zu construct=[%s] "
			            "gercek_cikis=%s ncpd=%u %s\n",
			            header, construct.size(), VectorToString(construct).c_str(),
			            true_exit == UINT32_MAX ? "COKLU/YOK"
			                                    : std::to_string(true_exit).c_str(),
			            merge, true_exit != UINT32_MAX && true_exit != merge ? "<-- NCPD ASIYOR" : "");
		}
		std::fflush(stdout);
	}
}

// DONGUDEN BREAK YAPAN SECIM (olculen B sinifi).
//
// Desen: blok 9 dongu basligi (merge=20), blok 10 dongunun ICINDE bir secim
// ve 10'un NCPD'si de 20 cikiyor. SPIR-V'de bir secim construct'i ile onu
// kapsayan dongu AYNI merge blogunu paylasamaz.
//
// Cozum: secimin construct'indan dongu merge'ine giden kenarlari (yani
// BREAK'leri) once kendi sentetik merge blogunda topla, oradan tek bir
// kosulsuz dallanma ile dongu merge'ine git. GERI KENARA DOKUNMAZ - olculdu:
// 10'un construct'indan cikan kenarlar 17->9 (geri kenar) ve 18->20 (break);
// yalnizca ikincisi yeniden kablolanir, dongu korunur.
//
// Yeniden kablolamayi SplitSharedMergeBlock zaten yapiyor; eksik olan onu
// SECIMIN construct'i ile cagirmakti. Disaridan cagrildiginda construct
// onculu [18], dis oncul [19] olur - ikisi de dolu, yani bolme calisir.
// IC ICE GECMIS IKI SECIM AYNI MERGE'I PAYLASIYORSA, ICTEKINE KENDI
// SENTETIK YONLENDIRICISINI VER.  (SONDA SURUMU - bkz. [CFG-IC-BOLME])
//
// Ilk denemede bu pas hatayi "Selection must be structured"a cevirdi. En
// olasi mekanizma: yaratilan sentetik blok ULASILAMAZ sayilip etiket
// alamiyor, `BlockLabel` 0 donuyor ve OpSelectionMerge HIC basilmiyor.
// Bu surum ayni isi yapiyor ama yarattigi blogun kablolamasini da dokuyor.
bool SplitOneNestedSelectionMerge(Graph* graph, std::vector<uint32_t>* already_split) {
	// VARSAYILAN KAPALI (PSEMU_CFG_ICBOLME=1).
	//
	// Pas ASIL ISINI YAPIYOR: merge cakismasi cozuluyor ve program
	// dispatcher'a HIC dusmuyor (0/32; kapaliyken 7/7). Ama baska bir yerde
	// gecersizlik doguruyor: "Selection must be structured", MS gecersiz=15,
	// MESH-CIZIM=0. Kapaliyken MS gecersiz=0 ve MESH-CIZIM=8.
	// Ikisini birden kaybetmemek icin bayrakla ayrildi.
	static const bool enabled = [] {
		const char* env = std::getenv("PSEMU_CFG_ICBOLME");
		return env != nullptr && env[0] == '1';
	}();
	if (!enabled) {
		return false;
	}

	const auto original_block_count = static_cast<uint32_t>(graph->blocks.size());

	std::map<uint32_t, std::vector<uint32_t>> claims;
	for (uint32_t block_id = 0; block_id < original_block_count; block_id++) {
		const auto* block = graph->FindBlock(block_id);
		if (block == nullptr || block->terminator.loop_header ||
		    block->terminator.kind != TerminatorKind::ConditionalBranch) {
			continue;
		}
		const auto merge = graph->FindNearestCommonPostDominator(block->terminator.true_block,
		                                                         block->terminator.false_block);
		if (merge != UINT32_MAX) {
			AddUnique(&claims[merge], block_id);
		}
	}

	// KUYRUK UZUNLUGU: kac merge birden fazla baslik tarafindan talep
	// ediliyor? "Birini coz, sirakini gor" dongusunun KAC HALKA surecegini
	// tahmin etmek yerine SAYIYORUZ.
	if (CfgTraceEnabled()) {
		uint32_t contested = 0;
		for (const auto& [merge, headers]: claims) {
			if (headers.size() >= 2) {
				contested++;
			}
		}
		if (contested != 0) {
			std::printf("[NESTED-KUYRUK] cakisan_merge=%u (toplam merge talebi=%zu)\n", contested,
			            claims.size());
			std::fflush(stdout);
		}
	}

	const bool trace = CfgTraceEnabled();
	const auto decline = [&](uint32_t inner, uint32_t merge, const char* reason) {
		if (trace) {
			std::printf("[NESTED-RED] inner=%u merge=%u neden=%s\n", inner, merge, reason);
			std::fflush(stdout);
		}
	};

	for (const auto& [merge, headers]: claims) {
		if (headers.size() < 2) {
			continue;
		}
		for (auto inner: headers) {
			if (std::find(already_split->begin(), already_split->end(), inner) !=
			    already_split->end()) {
				decline(inner, merge, "ZATEN_BOLUNDU");
				continue;
			}
			const bool nested = std::any_of(headers.begin(), headers.end(), [&](uint32_t other) {
				return other != inner && graph->Dominates(other, inner);
			});
			if (!nested) {
				decline(inner, merge, "DOMINANS");
				continue;
			}
			const auto* merge_block = graph->FindBlock(merge);
			if (merge_block == nullptr) {
				decline(inner, merge, "MERGE_BLOK_YOK");
				continue;
			}
			std::vector<uint32_t> preds_in;
			for (auto pred: merge_block->predecessors) {
				if (graph->Dominates(inner, pred)) {
					AddUnique(&preds_in, pred);
				}
			}
			const auto total_preds = merge_block->predecessors.size();
			if (preds_in.empty()) {
				decline(inner, merge, "ICERIDE_ONCUL_YOK");
				continue;
			}
			if (preds_in.size() == total_preds) {
				decline(inner, merge, "TUM_ONCULLER_ICERIDE");
				continue;
			}

			const auto synthetic = AppendSyntheticMergeBlock(graph, merge);
			if (auto* sb = graph->FindBlock(synthetic); sb != nullptr) {
				sb->predecessors = preds_in;
				SortUnique(&sb->predecessors);
			}
			for (auto pred: preds_in) {
				auto* b = graph->FindBlock(pred);
				if (b == nullptr) {
					continue;
				}
				ReplaceValue(&b->successors, merge, synthetic);
				ReplaceTerminatorTarget(&b->terminator, merge, synthetic);
			}
			if (auto* om = graph->FindBlock(merge); om != nullptr) {
				for (auto pred: preds_in) {
					RemoveValue(&om->predecessors, pred);
				}
				AddUnique(&om->predecessors, synthetic);
				SortUnique(&om->predecessors);
			}
			// SONDA: yaratilan blogun kablolamasi. Ardili yoksa ya da
			// onculu kalmadiysa "cikmaz sokak" sayilip budanir.
			if (const auto* sb = graph->FindBlock(synthetic); sb != nullptr) {
				std::string preds;
				for (auto p: sb->predecessors) {
					preds += fmt::format(" {}", p);
				}
				std::string succs;
				for (auto s: sb->successors) {
					succs += fmt::format(" {}", s);
				}
				std::printf("[CFG-IC-BOLME] ic_baslik=%u merge=%u -> sentetik=%u | tasinan=%zu/%zu "
				            "| sentetik.onculler=[%s] sentetik.ardillar=[%s] terminator_hedef=%u\n",
				            inner, merge, synthetic, preds_in.size(), total_preds, preds.c_str(),
				            succs.c_str(), sb->terminator.true_block);
				std::fflush(stdout);
			}
			// KARAR KORUMA - PARCA 1/3: kararı YAZ.
			// Pas simdiye kadar yalnizca onculleri yeniden bagliyordu ve
			// merge_block'a HIC dokunmuyordu; dolayisiyla korunacak bir karar
			// da yoktu. Atama asamasi NCPD'yi sifirdan hesaplayip yine
			// catisan blogu buluyordu (46 -> 50 -> 51).
			// MoveBlockBefore'dan ONCE yaziyoruz: remap merge_block alanini da
			// tasiyor (bkz. RemapId zinciri), yani kimlik kaymasi guvenli.
			if (auto* ih = graph->FindBlock(inner); ih != nullptr) {
				ih->terminator.merge_block = synthetic;
			}
			AddUnique(already_split, inner);
			MoveBlockBefore(graph, synthetic, merge);
			return true;
		}
	}
	return false;
}

// ASIRI YUKLU DONGU BASLIGI: bir blok hem OpLoopMerge hem OpSelectionMerge
// ilan edemez.
//
// Olculen yapi (disassembly, hash e8473143):
//   %229 = OpLabel
//          OpLoopMerge %263 %262 None      <- dongu merge=%263 continue=%262
//          OpBranchConditional %2087 %231 %230
// Hedefler %231 ve %230; IKISI DE dongu merge'i ya da continue'su DEGIL.
// Yani basligin icinde, dongunun sinirlariyla ilgisi olmayan BAGIMSIZ bir
// secim var. SPIR-V'de bir blok TEK bir yapi ilan edebilir; OpLoopMerge
// yeri doldurdugu icin secim yapisiz kaliyor -> "Selection must be
// structured".
//
// COZUM: basligi IKIYE AYIR. Baslik OpLoopMerge + KOSULSUZ dallanma ile yeni
// bloga gider; kosullu dallanma o yeni bloga tasinir ve orada kendi
// OpSelectionMerge'unu alir. Iki yapi, iki ayri blok.
//
// SONLANMA: her dongu basligi en cok bir kez bolunur.
bool SplitIndependentSelectionInLoopHeader(Graph* graph, std::vector<uint32_t>* already_split) {
	for (const auto& loop: graph->natural_loops) {
		const auto header_id = loop.header;
		if (std::find(already_split->begin(), already_split->end(), header_id) !=
		    already_split->end()) {
			continue;
		}
		auto* header = graph->FindBlock(header_id);
		if (header == nullptr || header->terminator.kind != TerminatorKind::ConditionalBranch) {
			continue;
		}
		const auto t = header->terminator.true_block;
		const auto f = header->terminator.false_block;
		// Dongu sinirina dokunan bir dal varsa bu MESRU dongu kosuludur.
		const auto touches_loop = [&](uint32_t b) {
			return b == UINT32_MAX || b == loop.merge || b == loop.continue_block ||
			       b == loop.header;
		};
		if (touches_loop(t) || touches_loop(f)) {
			continue;
		}

		// Secimi tasiyacak yeni blok. Baslikta KOD KALIR, yalnizca terminator
		// tasinir - blok govdesi bos olmalidir ki komutlar cift yayilmasin.
		BasicBlock selection;
		selection.id         = static_cast<uint32_t>(graph->blocks.size());
		selection.start_pc   = header->end_pc;
		selection.end_pc     = header->end_pc;
		selection.inst_begin = header->inst_end;
		selection.inst_end   = header->inst_end;
		selection.terminator = header->terminator;
		selection.successors = {t, f};
		SortUnique(&selection.successors);
		selection.predecessors = {header_id};
		const auto selection_id = selection.id;
		graph->blocks.push_back(std::move(selection));

		// Baslik artik yalnizca dongu yapisini ilan eder ve kosulsuz dallanir.
		header->terminator.kind        = TerminatorKind::Branch;
		header->terminator.condition   = BranchCondition::Always;
		header->terminator.true_block  = selection_id;
		header->terminator.false_block = UINT32_MAX;
		header->successors             = {selection_id};

		// Dallarin onculleri: baslik yerine yeni secim blogu.
		for (auto target: {t, f}) {
			if (auto* b = graph->FindBlock(target); b != nullptr) {
				RemoveValue(&b->predecessors, header_id);
				AddUnique(&b->predecessors, selection_id);
				SortUnique(&b->predecessors);
			}
		}

		std::printf("[CFG-BASLIK-BOLME] dongu_basligi=%u -> secim_blogu=%u | dallar=(%u,%u) "
		            "dongu(merge=%u continue=%u)\n",
		            header_id, selection_id, t, f, loop.merge, loop.continue_block);
		std::fflush(stdout);
		AddUnique(already_split, header_id);
		MoveBlockBefore(graph, selection_id, std::min(t, f));
		return true;
	}
	return false;
}

bool SplitOneLoopBreakSelection(Graph* graph, std::vector<uint32_t>* already_split) {
	std::vector<uint32_t> loop_headers;
	for (const auto& loop: graph->natural_loops) {
		AddUnique(&loop_headers, loop.header);
	}

	const auto original_block_count = static_cast<uint32_t>(graph->blocks.size());
	for (uint32_t block_id = 0; block_id < original_block_count; block_id++) {
		const auto* block = graph->FindBlock(block_id);
		// SONLANMA GARANTISI: her secim basligi EN COK BIR KEZ bolunur.
		// Olculdu: bu guard olmadan ayni secim (44) 108 kez bolundu ve butce
		// tukendi - bolme NCPD'yi degistirmediginde kosul tekrar saglaniyor.
		// Basliklar sonlu oldugu icin bu olcut sonlanmayi garanti eder.
		if (block == nullptr || block->terminator.kind != TerminatorKind::ConditionalBranch ||
		    Contains(loop_headers, block_id) || Contains(*already_split, block_id)) {
			continue;
		}
		const auto merge = graph->FindNearestCommonPostDominator(block->terminator.true_block,
		                                                         block->terminator.false_block);
		if (merge == UINT32_MAX) {
			continue;
		}
		// Kapsayan dongu: bu secimi domine eden bir dongu basligi ve dongunun
		// merge'i bizim NCPD'miz ile AYNI mi?
		for (const auto& loop: graph->natural_loops) {
			if (loop.merge != merge || loop.header == block_id ||
			    !graph->Dominates(loop.header, block_id)) {
				continue;
			}
			const auto construct = DominatedBlocks(*graph, block_id, merge);
			AddUnique(already_split, block_id);
			if (SplitSharedMergeBlock(graph, merge, construct, true)) {
				std::printf("[CFG-BREAK-BOLME] secim=%u dongu_basligi=%u dongu_merge=%u "
				            "construct_boyu=%zu -> sentetik merge eklendi\n",
				            block_id, loop.header, merge, construct.size());
				std::fflush(stdout);
				return true;
			}
			break;
		}
	}
	return false;
}

bool SplitSharedMergeBlocks(Graph* graph, std::string* error) {
	const auto original_block_count = static_cast<uint32_t>(graph->blocks.size());
	const auto split_budget =
	    std::max<uint32_t>(16u, std::min<uint32_t>(128u, original_block_count));
	// Break-bolmesi yapilmis secim basliklari; her biri en cok bir kez.
	std::vector<uint32_t> break_split_headers;
	std::vector<uint32_t> nested_split_headers;
	std::vector<uint32_t> header_split_headers;
	for (uint32_t splits = 0; splits < split_budget; splits++) {
		if (!SplitOneLoopMerge(graph) && !SplitOneSelectionMerge(graph) &&
		    !SplitOneLoopBreakSelection(graph, &break_split_headers) &&
		    !SplitIndependentSelectionInLoopHeader(graph, &header_split_headers) &&
		    !SplitOneNestedSelectionMerge(graph, &nested_split_headers)) {
			// Mevcut bolme pasleri tukendi. Geriye cakisma kaldiysa haritayi
			// dok - genellestirilmis cozum buradan yazilacak.
			LogMergeClaimMap(*graph);
			return true;
		}
		RebuildPredecessors(graph);
		RecomputeAnalyses(graph);
	}
	SetFailure(graph, FailureKind::StructuredControlFlow, graph->entry_block,
	           fmt::format("CFG shared merge splitting exceeded budget: original_blocks={} "
	                       "current_blocks={} split_budget={}",
	                       original_block_count, static_cast<uint64_t>(graph->blocks.size()),
	                       split_budget),
	           error);
	return false;
}

void ClearStructuredTerminators(Graph* graph) {
	// KARAR KORUMA - PARCA 2/3: kalkan.
	//
	// Bu fonksiyon KOSULSUZ siliyordu ve bolme paslarinin urettigi her karar
	// buharlasiyordu; atama asamasi sifirdan hesaplayip yine catisan blogu
	// buluyordu (cakisma 46 -> 50 -> 51 diye numara degistirip duruyordu).
	//
	// YALNIZCA SENTETIK YONLENDIRICILER korunur. "Bos degilse koru" demek
	// TEHLIKELI olurdu: `loop_header` bayragi da burada sifirlaniyor ve dongu
	// merge/continue'lari hemen sonra `natural_loops`'tan YENIDEN kuruluyor;
	// onlari korursak iki kaynak catisir.
	for (auto& block: graph->blocks) {
		const auto kept = block.terminator.merge_block;

		block.terminator.merge_block    = UINT32_MAX;
		block.terminator.continue_block = UINT32_MAX;
		block.terminator.loop_header    = false;

		if (kept != UINT32_MAX && IsSyntheticForwarderBlock(*graph, kept)) {
			block.terminator.merge_block = kept;
			if (CfgTraceEnabled()) {
				std::printf("[KARAR-KORUMA] blok=%u merge=%u KORUNDU (sentetik yonlendirici)\n",
				            block.id, kept);
				std::fflush(stdout);
			}
		}
	}
}

std::string VectorToString(const std::vector<uint32_t>& values) {
	std::string text;
	for (uint32_t i = 0; i < values.size(); i++) {
		if (i != 0) {
			text += ",";
		}
		text += fmt::format("{}", values[i]);
	}
	return text;
}

} // namespace

const BasicBlock* Graph::FindBlock(uint32_t id) const {
	if (id < blocks.size() && blocks[id].id == id) {
		return &blocks[id];
	}
	for (const auto& block: blocks) {
		if (block.id == id) {
			return &block;
		}
	}
	return nullptr;
}

BasicBlock* Graph::FindBlock(uint32_t id) {
	return const_cast<BasicBlock*>(static_cast<const Graph*>(this)->FindBlock(id));
}

const BasicBlock* Graph::FindBlockByPc(uint32_t pc) const {
	for (const auto& block: blocks) {
		if (block.start_pc == pc) {
			return &block;
		}
	}
	return nullptr;
}

BasicBlock* Graph::FindBlockByPc(uint32_t pc) {
	return const_cast<BasicBlock*>(static_cast<const Graph*>(this)->FindBlockByPc(pc));
}

bool Graph::Dominates(uint32_t dominator, uint32_t block) const {
	const auto* target = FindBlock(block);
	return target != nullptr && Contains(target->dominators, dominator);
}

bool Graph::PostDominates(uint32_t post_dominator, uint32_t block) const {
	const auto* target = FindBlock(block);
	return target != nullptr && Contains(target->post_dominators, post_dominator);
}

uint32_t Graph::FindNearestCommonPostDominator(uint32_t block_a, uint32_t block_b) const {
	const auto* a = FindBlock(block_a);
	const auto* b = FindBlock(block_b);
	if (a == nullptr || b == nullptr) {
		return UINT32_MAX;
	}

	const auto common = IntersectSorted(a->post_dominators, b->post_dominators);
	for (auto candidate: common) {
		bool nearest = true;
		for (auto other: common) {
			if (other != candidate && !PostDominates(other, candidate)) {
				nearest = false;
				break;
			}
		}
		if (nearest) {
			return candidate;
		}
	}
	return common.empty() ? UINT32_MAX : common.front();
}

bool BuildGraph(const Decoder::Program& program, Graph* graph, std::string* error) {
	if (graph == nullptr) {
		SetError(error, "invalid CFG output");
		return false;
	}
	*graph = {};

	if (program.instructions.empty()) {
		SetError(error, "cannot build CFG for empty shader");
		return false;
	}

	const auto first_pc = program.instructions.front().pc;
	const auto end_pc   = ProgramEndPc(program);

	std::set<uint32_t> instruction_pcs;
	for (const auto& inst: program.instructions) {
		instruction_pcs.insert(inst.pc);
		if (inst.opcode == Opcode::Unsupported) {
			SetFailure(graph, FailureKind::UnsupportedInstruction, UINT32_MAX,
			           fmt::format("unsupported decoded instruction in CFG at pc 0x{:08x}: {}",
			                       inst.pc, Decoder::InstructionToString(inst).c_str()),
			           error);
			return false;
		}
	}

	std::set<uint32_t> labels;
	labels.insert(first_pc);
	labels.insert(end_pc);

	std::map<uint32_t, SetpcTargetInfo> setpc_targets;
	bool                                indirect_setpc = false;
	for (uint32_t i = 0; i < program.instructions.size(); i++) {
		const auto& inst    = program.instructions[i];
		const auto  next_pc = InstructionEndPc(inst);
		if (IsBranch(inst.opcode)) {
			if (!IsValidTarget(inst.branch_target, instruction_pcs, first_pc, end_pc)) {
				SetFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				           fmt::format("branch at pc 0x{:08x} targets invalid pc 0x{:08x}", inst.pc,
				                       inst.branch_target),
				           error);
				return false;
			}
			labels.insert(inst.branch_target);
			if (next_pc <= end_pc) {
				labels.insert(next_pc);
			}
		} else if (inst.opcode == Opcode::SSetpcB64) {
			SetpcTargetInfo target_info;
			if (!ResolveSetpcTargets(program, i, &target_info)) {
				SetFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
				           fmt::format("unsupported dynamic S_SETPC_B64 at pc 0x{:08x}", inst.pc),
				           error);
				return false;
			}
			const auto& target_pcs = target_info.indirect
			                             ? target_info.target_pcs
			                             : std::vector<uint32_t> {target_info.target};
			for (const auto target: target_pcs) {
				if (!IsValidTarget(target, instruction_pcs, first_pc, end_pc)) {
					SetFailure(graph, FailureKind::InvalidBranchTarget, UINT32_MAX,
					           fmt::format("S_SETPC_B64 at pc 0x{:08x} targets invalid pc 0x{:08x}",
					                       inst.pc, target),
					           error);
					return false;
				}
				labels.insert(target);
			}
			indirect_setpc = indirect_setpc || target_info.indirect;
			setpc_targets.emplace(inst.pc, std::move(target_info));
			if (next_pc <= end_pc) {
				labels.insert(next_pc);
			}
		} else if (inst.opcode == Opcode::SEndpgm) {
			labels.insert(next_pc);
		}
	}

	std::vector<uint32_t> sorted_labels(labels.begin(), labels.end());
	std::sort(sorted_labels.begin(), sorted_labels.end());
	for (uint32_t i = 0; i < sorted_labels.size(); i++) {
		const auto start = sorted_labels[i];
		if (start > end_pc) {
			continue;
		}
		if (start != end_pc && !instruction_pcs.contains(start)) {
			SetFailure(graph, FailureKind::InvalidLabel, UINT32_MAX,
			           fmt::format("CFG label does not start on an instruction: 0x{:08x}", start),
			           error);
			return false;
		}

		BasicBlock block;
		block.id         = static_cast<uint32_t>(graph->blocks.size());
		block.start_pc   = start;
		block.end_pc     = i + 1u < sorted_labels.size() ? sorted_labels[i + 1u] : end_pc;
		block.inst_begin = static_cast<uint32_t>(
		    std::lower_bound(program.instructions.begin(), program.instructions.end(), start,
		                     [](const Instruction& inst, uint32_t pc) { return inst.pc < pc; }) -
		    program.instructions.begin());
		block.inst_end = static_cast<uint32_t>(
		    std::lower_bound(program.instructions.begin(), program.instructions.end(), block.end_pc,
		                     [](const Instruction& inst, uint32_t pc) { return inst.pc < pc; }) -
		    program.instructions.begin());
		graph->blocks.push_back(std::move(block));
	}

	graph->entry_block = 0;

	std::map<uint32_t, uint32_t> pc_to_block;
	for (const auto& block: graph->blocks) {
		pc_to_block.emplace(block.start_pc, block.id);
	}

	for (auto& block: graph->blocks) {
		block.terminator = {};
		if (block.inst_begin == block.inst_end) {
			block.terminator.kind = TerminatorKind::Return;
			continue;
		}

		const auto& last    = program.instructions[block.inst_end - 1u];
		const auto  next_pc = InstructionEndPc(last);
		if (last.opcode == Opcode::SEndpgm) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.condition  = BranchCondition::Always;
			block.terminator.true_block = pc_to_block.at(end_pc);
		} else if (last.opcode == Opcode::SSetpcB64) {
			const auto& target_info = setpc_targets.at(last.pc);
			if (target_info.indirect) {
				block.terminator.kind                   = TerminatorKind::IndirectBranch;
				block.terminator.condition              = BranchCondition::Always;
				block.terminator.indirect_pc_sgpr       = target_info.pc_sgpr;
				block.terminator.indirect_selector_code = target_info.selector_code;
				for (const auto target_pc: target_info.target_pcs) {
					block.terminator.indirect_target_pcs.push_back(target_pc);
					block.terminator.indirect_targets.push_back(pc_to_block.at(target_pc));
				}
				const auto selector_count = std::min(target_info.selector_values.size(),
				                                     target_info.selector_target_pcs.size());
				for (uint32_t i = 0; i < selector_count; i++) {
					block.terminator.indirect_selector_values.push_back(
					    target_info.selector_values[i]);
					block.terminator.indirect_selector_targets.push_back(
					    pc_to_block.at(target_info.selector_target_pcs[i]));
				}
				if (target_info.table_load_pc != UINT32_MAX) {
					AddUnique(&graph->code_table_load_pcs, target_info.table_load_pc);
				}
			} else {
				block.terminator.kind       = TerminatorKind::Branch;
				block.terminator.condition  = BranchCondition::Always;
				block.terminator.true_block = pc_to_block.at(target_info.target);
			}
		} else if (IsUnconditionalBranch(last.opcode)) {
			block.terminator.kind       = TerminatorKind::Branch;
			block.terminator.condition  = BranchCondition::Always;
			block.terminator.true_block = pc_to_block.at(last.branch_target);
		} else if (IsConditionalBranch(last.opcode)) {
			block.terminator.kind       = TerminatorKind::ConditionalBranch;
			block.terminator.condition  = ConditionForOpcode(last.opcode);
			block.terminator.true_block = pc_to_block.at(last.branch_target);
			const auto fallthrough      = pc_to_block.find(next_pc);
			if (fallthrough == pc_to_block.end()) {
				SetFailure(graph, FailureKind::MissingFallthrough, block.id,
				           fmt::format("conditional branch at pc 0x{:08x} has no fallthrough block",
				                       last.pc),
				           error);
				return false;
			}
			block.terminator.false_block = fallthrough->second;
		} else {
			auto next = pc_to_block.find(block.end_pc);
			if (next != pc_to_block.end() && block.end_pc != block.start_pc) {
				block.terminator.kind       = TerminatorKind::Branch;
				block.terminator.condition  = BranchCondition::Always;
				block.terminator.true_block = next->second;
			} else {
				block.terminator.kind = TerminatorKind::Return;
			}
		}

		switch (block.terminator.kind) {
			case TerminatorKind::Branch:
				AddUnique(&block.successors, block.terminator.true_block);
				break;
			case TerminatorKind::ConditionalBranch:
				AddUnique(&block.successors, block.terminator.true_block);
				AddUnique(&block.successors, block.terminator.false_block);
				break;
			case TerminatorKind::IndirectBranch:
				for (const auto target: block.terminator.indirect_targets) {
					AddUnique(&block.successors, target);
				}
				break;
			case TerminatorKind::Return:
			case TerminatorKind::Unsupported: break;
		}
	}

	for (auto& block: graph->blocks) {
		SortUnique(&block.successors);
		for (auto succ: block.successors) {
			AddUnique(&graph->blocks[succ].predecessors, block.id);
		}
	}
	for (auto& block: graph->blocks) {
		SortUnique(&block.predecessors);
	}
	SortUnique(&graph->code_table_load_pcs);

	ComputeDominators(graph);
	ComputePostDominators(graph);
	ComputeBackEdges(graph);
	ComputeNaturalLoops(graph);
	ComputeComponents(graph);

	if (indirect_setpc) {
		graph->irreducible        = true;
		graph->unsupported        = false;
		graph->failure_kind       = FailureKind::IrreducibleControlFlow;
		graph->unsupported_reason = "indirect S_SETPC_B64 jump table requires dispatcher fallback";
	}

	if (graph->irreducible) {
		graph->unsupported = false;
	}

	return true;
}

bool Structurize(Graph* graph, std::string* error) {
	if (graph == nullptr) {
		SetError(error, "invalid CFG input");
		return false;
	}
	if (graph->unsupported || graph->irreducible) {
		if (graph->unsupported_reason.empty()) {
			graph->unsupported_reason = "unsupported CFG";
		}
		SetError(error, graph->unsupported_reason);
		return false;
	}

	if (!SplitSharedMergeBlocks(graph, error)) {
		return false;
	}
	ClearStructuredTerminators(graph);

	std::map<uint32_t, uint32_t> merge_headers;
	// TANI: ilk cakismada durmak yerine HEPSINI sayiyoruz. "Birini coz,
	// sirakini gor" dongusune girmemek icin once toplam sayiyi ve siniflarini
	// bilmek gerekiyor.
	uint32_t conflict_count = 0;
	auto     reserve_merge_block = [&](uint32_t header, uint32_t merge) {
		const auto [it, inserted] = merge_headers.emplace(merge, header);
		if (!inserted && it->second != header) {
			conflict_count++;
			// TANI: cakismanin ANATOMISI. Iki basligin domine ettigi kumeler ve
			// merge'in oncul kesisimi yan yana. Olculdu: ic ice iki secim
			// (disaridaki + icerideki) ayni bloga yakinsiyor; icteki icin bolme,
			// tek construct onculunun ZATEN sentetik yonlendirici olmasi
			// yuzunden reddediliyor (bkz. [CFG-BOLME-RED]).
			const auto            other       = it->second;
			const auto*           merge_block = graph->FindBlock(merge);
			const auto            dom_new     = DominatedBlocks(*graph, header, merge);
			const auto            dom_old     = DominatedBlocks(*graph, other, merge);
			std::vector<uint32_t> preds_in_new;
			std::vector<uint32_t> preds_in_old;
			std::vector<uint32_t> preds_outside;
			if (merge_block != nullptr) {
				for (auto pred: merge_block->predecessors) {
					const bool in_new = Contains(dom_new, pred);
					const bool in_old = Contains(dom_old, pred);
					if (in_new) {
						AddUnique(&preds_in_new, pred);
					}
					if (in_old) {
						AddUnique(&preds_in_old, pred);
					}
					if (!in_new && !in_old) {
						AddUnique(&preds_outside, pred);
					}
				}
			}
			std::printf("[CFG-CAKISMA] merge=%u | yeni_header=%u eski_header=%u\n"
			            "  merge_onculleri = [%s]\n"
			            "  yeni_header domine (merge'e kadar) = [%s]\n"
			            "  eski_header domine (merge'e kadar) = [%s]\n"
			            "  oncul kesisimi: yeni=[%s] eski=[%s] disarida=[%s]\n"
			            "  dominans: yeni->merge=%d eski->merge=%d eski->yeni=%d\n",
			            merge, header, other,
			            merge_block != nullptr ? VectorToString(merge_block->predecessors).c_str()
			                                   : "?",
			            VectorToString(dom_new).c_str(), VectorToString(dom_old).c_str(),
			            VectorToString(preds_in_new).c_str(), VectorToString(preds_in_old).c_str(),
			            VectorToString(preds_outside).c_str(),
			            graph->Dominates(header, merge) ? 1 : 0,
			            graph->Dominates(other, merge) ? 1 : 0,
			            graph->Dominates(other, header) ? 1 : 0);
			// A sinifi mi B sinifi mi? Tek ve NCPD ile ayni cikis -> A.
			const auto construct_new = DominatedBlocks(*graph, header, merge);
			const auto exit_new      = FindTrueExitBlock(*graph, header, construct_new);
			std::printf("  SINIF=%s cakisma_no=%u gercek_cikis=%s construct_boyu=%zu\n",
			            exit_new == merge ? "A" : "B", conflict_count,
			            exit_new == UINT32_MAX ? "COKLU/YOK" : std::to_string(exit_new).c_str(),
			            construct_new.size());
			// Tek blokluk secim vakasi: her iki dal da AYNI yere mi gidiyor?
			// "Iki kenari da tek sentetik bloga topla" tasarimi ancak oyleyse
			// anlamsal olarak dogru; farkli hedefler varsa dallanmanin anlamini
			// yok eder.
			{
				const auto* h = graph->FindBlock(header);
				if (h != nullptr) {
					std::printf("  ARDILLAR: true=%u false=%u ayni=%d\n", h->terminator.true_block,
					            h->terminator.false_block,
					            h->terminator.true_block == h->terminator.false_block ? 1 : 0);
				}
			}
			// Bu basliklardan biri DONGU BASLIGI mi? Cikis kenarlarindan biri
			// geri kenarsa, ortada "coklu cikis" degil bir DONGU vardir ve
			// cakisma "donguden break yapan secim" desenidir.
			{
				bool  new_is_loop = false;
				bool  old_is_loop = false;
				for (const auto& loop: graph->natural_loops) {
					new_is_loop = new_is_loop || loop.header == header;
					old_is_loop = old_is_loop || loop.header == other;
				}
				std::printf("  DONGU: yeni_header=%d eski_header=%d\n", new_is_loop ? 1 : 0,
				            old_is_loop ? 1 : 0);
			}
			// B sinifi icin CIKIS KENARLARINI dok. "Sentetik hub" tasarimi
			// tum cikislari tek bir bloga toplayip oradan NCPD'ye dallandirmayi
			// ongoruyor; bu ancak cikislarin HEPSI zaten NCPD'ye gidiyorsa
			// anlamsal olarak dogru olur. Farkli hedefler varsa yeniden
			// kablolama sessizce yanlis kod uretir - once olcuyoruz.
			if (exit_new != merge) {
				std::printf("  CIKIS_KENARLARI:");
				for (auto id: construct_new) {
					const auto* b = graph->FindBlock(id);
					if (b == nullptr) {
						continue;
					}
					for (auto succ: b->successors) {
						if (!Contains(construct_new, succ)) {
							std::printf(" %u->%u", id, succ);
						}
					}
				}
				std::printf("\n");
			}
			std::fflush(stdout);
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, header,
			    fmt::format(
			        "duplicate structured merge block {} for header {} (already used by header {})",
			        merge, header, it->second),
			    error);
			return false;
		}
		return true;
	};

	for (const auto& loop: graph->natural_loops) {
		auto* header = graph->FindBlock(loop.header);
		if (header == nullptr || loop.merge == UINT32_MAX || loop.continue_block == UINT32_MAX) {
			SetFailure(
			    graph, FailureKind::StructuredControlFlow, loop.header,
			    fmt::format("loop at block {} has no structured merge/continue", loop.header),
			    error);
			return false;
		}
		if (!reserve_merge_block(loop.header, loop.merge)) {
			return false;
		}
		header->terminator.loop_header    = true;
		header->terminator.merge_block    = loop.merge;
		header->terminator.continue_block = loop.continue_block;
		if (CfgTraceEnabled()) {
			std::printf("[ATAMA] id=%u nereye=LoopMerge (baslik=%u)\n", loop.merge, loop.header);
			std::printf("[ATAMA] id=%u nereye=LoopContinue (baslik=%u)\n", loop.continue_block,
			            loop.header);
			std::fflush(stdout);
		}
	}

	for (auto& block: graph->blocks) {
		if (block.terminator.kind != TerminatorKind::ConditionalBranch ||
		    block.terminator.loop_header) {
			continue;
		}

		// DONGU LATCH'I SECIM DEGILDIR.
		//
		// Olculdu: blok 44'un dallari true=11 (dongu basligi) ve false=45
		// (dongu merge'i) - yani "if (kosul) continue; else break;". Bu bir
		// secim construct'i degil, dongunun geri kenar sonlandiricisidir ve
		// SPIR-V'de OpSelectionMerge almaz. Merge rezerve etmeye calisinca
		// dongunun merge'iyle cakisiyordu (44/11 -> 45, 42/9 -> 43).
		//
		// Tanim: ardillarindan biri, bu blogu domine eden bir dongu basligi.
		//
		// GENISLETME (olculdu): yukaridaki kural YETMIYOR. Dallar dogrudan
		// BASLIGA gitmek zorunda degil - dongunun MERGE ya da CONTINUE
		// blogana da gidebilirler. Olculen vaka (mesh, hash e8473143):
		//
		//   baslik  ic=5  dogru=ic45 yanlis=ic6  merge=ic44
		//   ic11    DONGU BASLIGI    merge=ic45  continue=ic44
		//
		// Yani ic5'teki secim, dogru dali olarak dongunun MERGE'ini (ic45 =
		// `break`), kendi merge'i olarak da dongunun CONTINUE'sunu (ic44 =
		// `continue`) sahipleniyordu. Ikisi de DONGU yapisina ait; SPIR-V'de
		// bir blok yalnizca TEK bir yapinin merge'i olabilir. Sonuc:
		// spirv-val "block '265' exits the selection headed by '696', but
		// not via a structured exit".
		//
		// Ilke: bir dal `break`, digeri `continue` ise o kollar BIRBIRIYLE
		// hicbir zaman birlesmez - biri dongunun disina cikar, digeri basina
		// sarar. Ortada bir secim birlesmesi YOKTUR, dolayisiyla
		// OpSelectionMerge de olmamalidir.
		{
			// KAPSAM SONDASI: genisletilmis kural ILK DENEMEDE ISE YARAMADI ve
			// hata BIREBIR ayni kaldi. Kuralı yazarken "bu blok dongunun
			// icinde" diye VARSAYDIM, olcmedim. Burasi tam olarak o varsayimi
			// sinar: her dongu icin dominans ve uc hedefin eslesme durumu.
			static const bool trace = [] {
				const char* env = std::getenv("PSEMU_CFG_HARITA");
				return env != nullptr && env[0] == '1';
			}();

			bool is_latch = false;
			for (const auto& loop: graph->natural_loops) {
				const bool dominated = graph->Dominates(loop.header, block.id);
				const auto targets_loop_structure = [&](uint32_t successor) {
					return successor != UINT32_MAX &&
					       (successor == loop.header || successor == loop.merge ||
					        successor == loop.continue_block);
				};
				const bool hits = targets_loop_structure(block.terminator.true_block) ||
				                  targets_loop_structure(block.terminator.false_block);
				if (trace) {
					std::printf("[CFG-LATCH] blok=%u dogru=%u yanlis=%u | dongu(baslik=%u "
					            "latch=%u merge=%u continue=%u) domine=%d hedef_tuttu=%d\n",
					            block.id, block.terminator.true_block,
					            block.terminator.false_block, loop.header, loop.latch, loop.merge,
					            loop.continue_block, dominated ? 1 : 0, hits ? 1 : 0);
					std::fflush(stdout);
				}
				if (dominated && hits) {
					is_latch = true;
					break;
				}
			}
			if (is_latch) {
				continue;
			}
		}

		// KARAR KORUMA - PARCA 3/3: saygi.
		// Temizlikten SAG KURTULMUS bir merge varsa, o bir bolme pasinin
		// bilincli karari demektir (kalkan yalnizca sentetik yonlendiricileri
		// birakiyor). NCPD'yi hic cagirma; yeniden hesaplamak tam da catismayi
		// geri getiren seydi.
		if (block.terminator.merge_block != UINT32_MAX) {
			if (!reserve_merge_block(block.id, block.terminator.merge_block)) {
				return false;
			}
			if (CfgTraceEnabled()) {
				std::printf("[KARAR-KORUMA] blok=%u korunan merge=%u kabul edildi (NCPD "
				            "atlandi)\n",
				            block.id, block.terminator.merge_block);
				std::fflush(stdout);
			}
			continue;
		}

		auto merge = graph->FindNearestCommonPostDominator(block.terminator.true_block,
		                                                   block.terminator.false_block);
		if (merge == UINT32_MAX) {
			SetFailure(graph, FailureKind::StructuredControlFlow, block.id,
			           fmt::format("conditional block {} has no structured merge", block.id),
			           error);
			return false;
		}

		// A SINIFI CAKISMA COZUMU: ic ice iki secim ayni merge'i talep ettiginde,
		// ICTEKI icin zaten var olan sentetik yonlendiriciyi merge yap. Yeni
		// blok URETMEZ, dolayisiyla sonlanir.
		//
		// Yalnizca basligin TEK ve NCPD ile ayni gercek cikisi varsa uygulanir.
		// Dongu-break deseni (B sinifi) artik SplitOneLoopBreakSelection ile
		// bolme asamasinda cozuluyor ve buraya ulasmiyor.
		if (const auto claimed = merge_headers.find(merge);
		    claimed != merge_headers.end() && claimed->second != block.id) {
			const auto construct = DominatedBlocks(*graph, block.id, merge);
			if (FindTrueExitBlock(*graph, block.id, construct) == merge) {
				const auto* merge_block = graph->FindBlock(merge);
				if (merge_block != nullptr) {
					for (auto pred: merge_block->predecessors) {
						if (!IsSyntheticMergeForwarder(*graph, pred, merge) ||
						    !graph->Dominates(block.id, pred) ||
						    merge_headers.find(pred) != merge_headers.end()) {
							continue;
						}
						// POST-DOMINANCE SARTI - BU KOSUL EKSIKTI, HATANIN
						// KAYNAGI BUYDU (bu oturumda yazilan A-sinifi yamasi).
						//
						// Olculdu: `[CFG-YENIDEN-HEDEF] header=5 merge 46 -> 44`.
						// NCPD DOGRU cevabi (46) vermisti; burasi onu 46'nin
						// oncalu olan sentetik yonlendirici 44'e cevirdi. Ama
						// 44 yalnizca YANLIS dal uzerinde; DOGRU dal (45) ona
						// hic ugramadan 46'ya atliyor. Yani 44 bir birlesme
						// noktasi degil, sadece bir yol ustundeki durak ->
						// spirv-val: "block '265' exits the selection headed
						// by '696', but not via a structured exit".
						//
						// Bir merge adayi HER IKI DALI DA post-domine
						// etmelidir; aksi halde o dal merge'e ugramadan
						// yapidan kacar.
						const auto post_dominates_branch = [&](uint32_t branch) {
							return branch == UINT32_MAX || branch == pred ||
							       graph->PostDominates(pred, branch);
						};
						if (!post_dominates_branch(block.terminator.true_block) ||
						    !post_dominates_branch(block.terminator.false_block)) {
							if (CfgTraceEnabled()) {
								std::printf("[CFG-YENIDEN-HEDEF] RED: header=%u aday=%u dallari "
								            "post-domine etmiyor (dogru=%u yanlis=%u)\n",
								            block.id, pred, block.terminator.true_block,
								            block.terminator.false_block);
								std::fflush(stdout);
							}
							continue;
						}
						std::printf("[CFG-YENIDEN-HEDEF] A-sinifi: header=%u merge %u -> %u\n",
						            block.id, merge, pred);
						std::fflush(stdout);
						merge = pred;
						break;
					}
				}
			}
		}

		if (!reserve_merge_block(block.id, merge)) {
			return false;
		}
		block.terminator.merge_block = merge;
		if (CfgTraceEnabled()) {
			std::printf("[ATAMA] id=%u nereye=SelectionMerge (baslik=%u dogru=%u yanlis=%u)\n",
			            merge, block.id, block.terminator.true_block,
			            block.terminator.false_block);
			std::fflush(stdout);
		}
	}

	// NIHAI DENETIM - YAYIM UZAYINDA.
	//
	// Butun paslar bitti; grafik artik emitter'in gorecegi haldedir, yani
	// spirv-val'in kimlikleriyle AYNI uzayda. Cakismayi burada ararsak
	// hicbir ceviriye ihtiyac kalmaz. SPIR-V kurali: bir blok yalnizca TEK
	// bir yapinin merge'i olabilir; ayrica bir dongunun merge/continue'su
	// baska bir yapinin merge'i olamaz.
	if (CfgTraceEnabled()) {
		std::map<uint32_t, std::string> roles;
		const auto claim = [&](uint32_t id, const char* role, uint32_t owner) {
			if (id == UINT32_MAX) {
				return;
			}
			auto& entry = roles[id];
			const auto text = fmt::format("{}(baslik={})", role, owner);
			if (!entry.empty()) {
				std::printf("[CAKISMA] blok=%u ZATEN %s iken simdi %s\n", id, entry.c_str(),
				            text.c_str());
				std::fflush(stdout);
				entry += " + ";
			}
			entry += text;
		};
		for (const auto& block: graph->blocks) {
			if (block.terminator.loop_header) {
				claim(block.terminator.merge_block, "LoopMerge", block.id);
				claim(block.terminator.continue_block, "LoopContinue", block.id);
			} else if (block.terminator.kind == TerminatorKind::ConditionalBranch) {
				claim(block.terminator.merge_block, "SelectionMerge", block.id);
			}
		}
		std::printf("[NIHAI] yayim uzayinda blok=%zu, rol sahibi=%zu\n", graph->blocks.size(),
		            roles.size());

		// MERGE GECERLILIK DENETIMI.
		//
		// Iki rakip teori var ve bu denetim onlari ayirir:
		//   (a) sucLU BOLUNME - bolen komut basligi ikiye ayiriyor
		//   (b) sucLU MERGE SECIMI - CFG, yapinin ICINE geri dallanan bir
		//       blogu merge ilan ediyor
		// Olculen supheli: %264 seçimin merge'i ama %265'e (kendi DOGRU
		// dalina) dallaniyor. Bir merge blogu kendi construct'inin icine
		// donuyorsa post-dominate etmiyordur ve yapi bozuktur.
		//
		// DOGRU DEGISMEZ: merge, HER IKI DALI DA POST-DOMINE ETMELI.
		//
		// Ilk denemede yanlis seyi sinadim ("merge construct icine geri
		// dallaniyor mu") ve 0 cikinca teoriyi curuk sandim. Disassembly'nin
		// statik izi gercegi gosterdi:
		//   %696 = OpSelectionMerge %264 / OpBranchConditional %756 %265 %226
		//   %265 = OpBranch %266     <- DOGRU DAL, %264'e HIC UGRAMADAN
		//   %264 = OpBranch %266        DIS secimin merge'ine atliyor
		// %265'in hicbir yolu %264'ten gecmiyor; yani %264 o dali post-domine
		// ETMIYOR ve merge olarak SAHTE.
		//
		// Gercek birlesme noktasi %266 ama o zaten DIS secimin merge'i - bir
		// blok yalnizca TEK bir yapinin merge'i olabilir. Dogru hamle,
		// AppendSyntheticMergeBlock ile araya bir yonlendirici koymakti;
		// yapilmamis, yerine rastgele bir blok merge ilan edilmis.
		uint32_t checked = 0;
		uint32_t fake    = 0;
		for (const auto& block: graph->blocks) {
			if (block.terminator.loop_header ||
			    block.terminator.kind != TerminatorKind::ConditionalBranch ||
			    block.terminator.merge_block == UINT32_MAX) {
				continue;
			}
			checked++;
			const auto merge = block.terminator.merge_block;
			const auto bad_branch = [&](uint32_t branch, const char* name) {
				if (branch == UINT32_MAX || branch == merge) {
					return false;
				}
				if (graph->PostDominates(merge, branch)) {
					return false;
				}
				std::printf("[MERGE-BOZUK] baslik=%u merge=%u | %s dali=%u -> merge o dali "
				            "POST-DOMINE ETMIYOR\n",
				            block.id, merge, name, branch);
				std::fflush(stdout);
				return true;
			};
			const bool t = bad_branch(block.terminator.true_block, "dogru");
			const bool f = bad_branch(block.terminator.false_block, "yanlis");
			if (t || f) {
				fake++;
			}
		}
		std::printf("[MERGE-BOZUK] ozet: secim=%u sahte_merge=%u\n", checked, fake);
		std::fflush(stdout);
	}

	return true;
}

std::string BranchConditionToString(BranchCondition condition) {
	switch (condition) {
		case BranchCondition::Always: return "always";
		case BranchCondition::SccZero: return "scc0";
		case BranchCondition::SccNonZero: return "scc1";
		case BranchCondition::VccZero: return "vccz";
		case BranchCondition::VccNonZero: return "vccnz";
		case BranchCondition::ExecZero: return "execz";
		case BranchCondition::ExecNonZero: return "execnz";
		default: return "unknown";
	}
}

std::string FailureKindToString(FailureKind kind) {
	switch (kind) {
		case FailureKind::None: return "None";
		case FailureKind::InvalidInput: return "InvalidInput";
		case FailureKind::UnsupportedInstruction: return "UnsupportedInstruction";
		case FailureKind::InvalidBranchTarget: return "InvalidBranchTarget";
		case FailureKind::MissingFallthrough: return "MissingFallthrough";
		case FailureKind::InvalidLabel: return "InvalidLabel";
		case FailureKind::IrreducibleControlFlow: return "IrreducibleControlFlow";
		case FailureKind::StructuredControlFlow: return "StructuredControlFlow";
		default: return "Unknown";
	}
}

std::string FormatBlockDiagnostic(const Graph& graph, uint32_t block_id, const char* stage,
                                  const std::string& reason) {
	const auto* block      = graph.FindBlock(block_id);
	const char* stage_text = stage != nullptr ? stage : "unknown";
	if (block == nullptr) {
		return fmt::format("stage={} block={} reason={}", stage_text, block_id, reason.c_str());
	}
	return fmt::format("stage={} block={} pc=0x{:08x}..0x{:08x} preds={} succs={} reason={}",
	                   stage_text, block->id, block->start_pc, block->end_pc,
	                   static_cast<uint32_t>(block->predecessors.size()),
	                   static_cast<uint32_t>(block->successors.size()), reason.c_str());
}

std::string GraphToString(const Graph& graph) {
	std::string text;
	text +=
	    fmt::format("entry_block={} irreducible={} unsupported={} failure={} failure_block={}\n",
	                graph.entry_block, graph.irreducible ? 1u : 0u, graph.unsupported ? 1u : 0u,
	                FailureKindToString(graph.failure_kind).c_str(), graph.failure_block);
	if (!graph.unsupported_reason.empty()) {
		text += "unsupported_reason=";
		text += graph.unsupported_reason;
		text += "\n";
	}

	for (const auto& block: graph.blocks) {
		text += fmt::format("block_{} pc=0x{:08x} end=0x{:08x} inst=[{},{})\n", block.id,
		                    block.start_pc, block.end_pc, block.inst_begin, block.inst_end);
		text += fmt::format("  predecessors=[{}] successors=[{}]\n",
		                    VectorToString(block.predecessors).c_str(),
		                    VectorToString(block.successors).c_str());
		text += fmt::format("  dominators=[{}] post_dominators=[{}]\n",
		                    VectorToString(block.dominators).c_str(),
		                    VectorToString(block.post_dominators).c_str());
		text += fmt::format(
		    "  terminator={} condition={} true={} false={} merge={} continue={} loop_header={} "
		    "indirect_sgpr={} indirect_selector={} indirect_targets=[{}] selector_values=[{}]\n",
		    static_cast<uint32_t>(block.terminator.kind),
		    BranchConditionToString(block.terminator.condition).c_str(),
		    block.terminator.true_block, block.terminator.false_block, block.terminator.merge_block,
		    block.terminator.continue_block, block.terminator.loop_header ? 1u : 0u,
		    block.terminator.indirect_pc_sgpr, block.terminator.indirect_selector_code,
		    VectorToString(block.terminator.indirect_targets).c_str(),
		    VectorToString(block.terminator.indirect_selector_values).c_str());
	}

	for (const auto& edge: graph.back_edges) {
		text += fmt::format("backedge {} -> {} natural={}\n", edge.from, edge.to,
		                    edge.natural ? 1u : 0u);
	}
	for (const auto& loop: graph.natural_loops) {
		text += fmt::format("loop header={} latch={} merge={} continue={} body=[{}] exits=[{}]\n",
		                    loop.header, loop.latch, loop.merge, loop.continue_block,
		                    VectorToString(loop.body_blocks).c_str(),
		                    VectorToString(loop.exit_blocks).c_str());
	}
	for (const auto& component: graph.components) {
		text += fmt::format("scc blocks=[{}] entries=[{}] irreducible={}\n",
		                    VectorToString(component.blocks).c_str(),
		                    VectorToString(component.entry_blocks).c_str(),
		                    component.irreducible ? 1u : 0u);
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::CFG
