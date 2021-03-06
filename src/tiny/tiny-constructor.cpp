#include "tiny/tiny-constructor.h"

TinyConstructor::TinyConstructor(Params& params) :
  Tiny(params),
  ot_snd(params, true),  //The true flag ensures that lsb(global_delta) == 1
  rot_seeds0(std::make_unique<uint8_t[]>(2 * CODEWORD_BITS * CSEC_BYTES)),
  rot_seeds1(rot_seeds0.get() + CODEWORD_BITS * CSEC_BYTES),
  raw_eval_ids(std::make_unique<uint32_t[]>(params.num_eval_gates + params.num_eval_auths)) {

  //The global eval_gate and eval_auth mappings. The below executions populate these arrays as the ids are received.
  eval_gates_ids = raw_eval_ids.get();
  eval_auths_ids = eval_gates_ids + params.num_eval_gates;
}

void TinyConstructor::Setup() {
  //=========================Run DOT===========================================
  auto baseOT_begin = GET_TIME();
  ot_snd.InitOTSender();
  auto baseOT_end = GET_TIME();

#ifdef TINY_PRINT
  PRINT_TIME(baseOT_end, baseOT_begin, "BASEOT");
#endif
}

void TinyConstructor::Preprocess() {
  std::vector<std::vector<std::chrono::duration<long double, std::milli>>> durations(CONST_NUM_TIMINGS);

  for (std::vector<std::chrono::duration<long double, std::milli>>& duration : durations) {
    duration.resize(params.num_execs);
  }

  auto setup_begin = GET_TIME();
  auto dot_begin = GET_TIME();
  //Run DOT Extension.
  ot_snd.Send();
  //Pointer for global delta for convenience
  global_delta = ot_snd.delta_outer.get();

  //Construct the ROTs necessary for commitment scheme setup. Requires computing base_outer \xor global_delta and then hashing both values into seeds_rot to remove the correlation
  uint8_t base_outer_delta[CSEC_BYTES];
  int rot_start_pos = params.num_OT - CODEWORD_BITS;
  for (int i = 0; i < CODEWORD_BITS; ++i) {
    XOR_128(base_outer_delta, ot_snd.base_outer.get() + (rot_start_pos + i) * CSEC_BYTES, global_delta);
    params.crypt.hash(rot_seeds0.get() + i * CSEC_BYTES, CSEC_BYTES, ot_snd.base_outer.get() + (rot_start_pos + i) * CSEC_BYTES, CSEC_BYTES);
    params.crypt.hash(rot_seeds1 + i * CSEC_BYTES, CSEC_BYTES, base_outer_delta, CSEC_BYTES);
  }

  auto dot_end = GET_TIME();
#ifdef TINY_PRINT
  PRINT_TIME(dot_end, dot_begin, "DOT");
#endif
  // =============================Run Commit===================================

  //Containers for holding pointers to objects used in each exec. For future use
  std::vector<std::future<void>> cnc_execs_finished(params.num_execs);
  std::unique_ptr<uint32_t[]> tmp_gate_eval_ids_ptr(new uint32_t[params.num_eval_gates + params.num_eval_auths]);
  uint32_t* tmp_gate_eval_ids = tmp_gate_eval_ids_ptr.get();
  uint32_t* tmp_auth_eval_ids = tmp_gate_eval_ids + params.num_eval_gates;

  //Split the number of preprocessed gates and inputs into num_execs executions
  std::vector<int> inputs_from, inputs_to, outputs_from, outputs_to, gates_from, gates_to, gates_inputs_from, gates_inputs_to;
  PartitionBufferFixedNum(inputs_from, inputs_to, params.num_execs, params.num_pre_inputs);
  PartitionBufferFixedNum(gates_inputs_from, gates_inputs_to, params.num_execs, params.num_pre_inputs / 2);
  PartitionBufferFixedNum(outputs_from, outputs_to, params.num_execs, params.num_pre_outputs);
  PartitionBufferFixedNum(gates_from, gates_to, params.num_execs, params.num_pre_gates);

  //Concurrency variables used for ensuring that exec_num 0 has sent and updated its global_delta commitment. This is needed as all other executions will use the same commitment to global_delta (in exec_num 0).
  std::mutex cout_mutex;
  std::mutex delta_updated_mutex;
  std::condition_variable delta_updated_cond_val;
  bool delta_updated = false;
  std::tuple<std::mutex&, std::condition_variable&, bool&> delta_checks = make_tuple(std::ref(delta_updated_mutex), std::ref(delta_updated_cond_val), std::ref(delta_updated));

  //store last exec_id as this execution performs the Delta-OT CnC step. This step is needed to ensure that the sender indeed committed to the global_delta used in DOT protocol.
  int last_exec_id = params.num_execs - 1;
  for (int exec_id = 0; exec_id < params.num_execs; ++exec_id) {

    //Assign pr. exec variables that are passed along to the current execution thread
    int inp_from = inputs_from[exec_id];
    int inp_to = inputs_to[exec_id];
    int thread_num_pre_inputs = inp_to - inp_from;
    int thread_num_pre_outputs = outputs_to[exec_id] - outputs_from[exec_id];
    int thread_num_pre_gates = gates_to[exec_id] - gates_from[exec_id];

    //Need to create a new params for each execution with the correct num_pre_gates and num_pre_inputs. The exec_id value decides which channel the execution is communicating on, so must match the eval execution.
    thread_params_vec.emplace_back(std::make_unique<Params>(params, thread_seeds.get() + exec_id * CSEC_BYTES, thread_num_pre_gates, thread_num_pre_inputs, thread_num_pre_outputs, exec_id));
    Params* thread_params = thread_params_vec[exec_id].get();

    //We store our local state in the containers as we need to access them for future use
    commit_snds.emplace_back(std::make_unique<CommitSender>(*thread_params, rot_seeds0.get(), rot_seeds1));
    CommitSender* commit_snd = commit_snds[exec_id].get();

    //The delta_holder is fixed and passed to all executions as it is in exec 0 that we commit to the global_delta and all other executions use this commitment when needed
    CommitSender* delta_holder = commit_snds[0].get();

    //Starts the current execution
    cnc_execs_finished[exec_id] = thread_pool.push([this, thread_params, commit_snd, delta_holder, exec_id, &cout_mutex, &delta_checks, inp_from, inp_to, last_exec_id, &durations, tmp_auth_eval_ids, tmp_gate_eval_ids] (int id) {

      auto commit_begin = GET_TIME();
      // If it's the last execution then we commit to s extra OTs as these are to be used for CNC.
      int num_OT_commits = inp_to - inp_from;
      uint8_t* inp_from_pointer = ot_snd.base_outer.get() + inp_from * CSEC_BYTES;
      if (exec_id == last_exec_id) {
        num_OT_commits += SSEC;
        thread_params->num_commits += SSEC;
      }

      commit_snd->Commit();

      //Do chosen commit to all DOT commitments.
      std::vector<uint64_t> ot_chosen_start_vec(num_OT_commits);
      std::iota(std::begin(ot_chosen_start_vec), std::end(ot_chosen_start_vec), thread_params->ot_chosen_start);
      commit_snd->ChosenCommit(inp_from_pointer, ot_chosen_start_vec, num_OT_commits);
      auto commit_end = GET_TIME();
      durations[CONST_COMMIT_TIME][exec_id] = commit_end - commit_begin;

      //Put global_delta from OTs in delta_pos of commitment scheme. For security reasons we only do this in exec_num 0, as else a malicious sender might send different delta values in each threaded execution. Therefore only exec_num 0 gets a correction and the rest simply update their delta pointer to point into exec_num 0's delta value.
      std::condition_variable& delta_updated_cond_val = std::get<1>(delta_checks);
      bool& delta_updated = std::get<2>(delta_checks);

      if (exec_id == 0) {
        uint8_t current_delta[CSEC_BYTES];
        XOR_128(current_delta, commit_snd->commit_shares0[thread_params->delta_pos], commit_snd->commit_shares1[thread_params->delta_pos]);

        uint8_t c[2 * BCH_BYTES] = {0};
        uint8_t* c_delta = c + BCH_BYTES;

        commit_snd->code->Encode(current_delta, c);
        commit_snd->code->Encode(global_delta, c_delta);

        uint8_t correction_commit_delta[CODEWORD_BYTES];
        XOR_128(correction_commit_delta, current_delta, global_delta);
        XOR_CheckBits(correction_commit_delta + CSEC_BYTES, c, c_delta);

        thread_params->chan.Send(correction_commit_delta, CODEWORD_BYTES);

        XOR_128(commit_snd->commit_shares1[thread_params->delta_pos], commit_snd->commit_shares0[thread_params->delta_pos], global_delta);
        XOR_CheckBits(commit_snd->commit_shares1[thread_params->delta_pos] + CSEC_BYTES, commit_snd->commit_shares0[thread_params->delta_pos] + CSEC_BYTES, c_delta);

        delta_updated = true;
        delta_updated_cond_val.notify_all();


      } else {

        std::mutex& delta_updated_mutex = std::get<0>(delta_checks);
        unique_lock<mutex> lock(delta_updated_mutex);
        while (!delta_updated) {
          delta_updated_cond_val.wait(lock);
        }
        commit_snd->commit_shares0[thread_params->delta_pos] = delta_holder->commit_shares0[delta_holder->params.delta_pos];
        commit_snd->commit_shares1[thread_params->delta_pos] = delta_holder->commit_shares1[delta_holder->params.delta_pos];
      }

      //If in the last execution we do CNC on the global_delta to ensure that this is indeed the global delta used in DOT as well
      if (exec_id == last_exec_id) {
        std::unique_ptr<uint8_t[]> cnc_ot_values(std::make_unique<uint8_t[]>(SSEC * CSEC_BYTES + SSEC_BYTES));
        thread_params->chan.ReceiveBlocking(cnc_ot_values.get(), SSEC * CSEC_BYTES + SSEC_BYTES);
        uint8_t* cnc_ot_choices = cnc_ot_values.get() + SSEC * CSEC_BYTES;

        //First test that receiver has sent valid OTs is trying to cheat. In the same loop we also build the decommit information.
        uint8_t correct_ot_value[CSEC_BYTES];

        std::unique_ptr<uint8_t[]> chosen_decommit_shares0(std::make_unique<uint8_t[]>(SSEC * (CODEWORD_BYTES + CSEC_BYTES)));
        uint8_t* chosen_decommit_shares1 = chosen_decommit_shares0.get() + SSEC * CODEWORD_BYTES;

        for (int i = 0; i < SSEC; ++i) {
          int commit_id = thread_params->ot_chosen_start + num_OT_commits - SSEC + i;
          std::copy(commit_snd->commit_shares0[commit_id], commit_snd->commit_shares0[commit_id] + CODEWORD_BYTES, chosen_decommit_shares0.get() + i * CODEWORD_BYTES);
          std::copy(commit_snd->commit_shares1[commit_id], commit_snd->commit_shares1[commit_id] + CSEC_BYTES, chosen_decommit_shares1 + i * CSEC_BYTES);
          if (GetBit(i, cnc_ot_choices)) {

            XOR_128(correct_ot_value, ot_snd.base_outer.get() + (inp_to + i) * CSEC_BYTES, global_delta);

            XOR_CodeWords(chosen_decommit_shares0.get() + i * CODEWORD_BYTES, commit_snd->commit_shares0[thread_params->delta_pos]);
            XOR_128(chosen_decommit_shares1 + i * CSEC_BYTES, commit_snd->commit_shares1[thread_params->delta_pos]);

          } else {
            std::copy(ot_snd.base_outer.get() + (inp_to + i) * CSEC_BYTES, ot_snd.base_outer.get() + (inp_to + i) * CSEC_BYTES + CSEC_BYTES, correct_ot_value);
          }
          if (!equal(correct_ot_value, correct_ot_value + CSEC_BYTES,  cnc_ot_values.get() + i * CSEC_BYTES)) {
            throw std::runtime_error("Receiver cheating. Trying to make us open to wrong OT!");
          }
        }

        //ChosenDecommit
        thread_params->chan.Send(chosen_decommit_shares0.get(), SSEC * (CODEWORD_BYTES + CSEC_BYTES));
      }

      //===========================VER_LEAK====================================
      auto verleak_begin = GET_TIME();
      int num_verleaks = thread_params->num_pre_outputs + thread_params->num_pre_inputs;

      std::unique_ptr<uint8_t[]> verleak_decommit_shares0(std::make_unique<uint8_t[]>(2 * (num_verleaks + AES_BITS) * CODEWORD_BYTES + BITS_TO_BYTES((num_verleaks + AES_BITS))));

      uint8_t* verleak_decommit_shares1 = verleak_decommit_shares0.get() + (num_verleaks + AES_BITS) * CODEWORD_BYTES;
      uint8_t* verleak_bits = verleak_decommit_shares1 + (num_verleaks + AES_BITS) * CODEWORD_BYTES;
      for (int i = 0; i < num_verleaks; ++i) {
        std::copy(commit_snd->commit_shares0[thread_params->out_lsb_blind_start + i], commit_snd->commit_shares0[thread_params->out_lsb_blind_start + i] + CODEWORD_BYTES, verleak_decommit_shares0.get() + i * CODEWORD_BYTES);
        std::copy(commit_snd->commit_shares1[thread_params->out_lsb_blind_start + i], commit_snd->commit_shares1[thread_params->out_lsb_blind_start + i] + CODEWORD_BYTES, verleak_decommit_shares1 + i * CODEWORD_BYTES);
        SetBit(i, GetLSB(commit_snd->commit_shares0[thread_params->out_lsb_blind_start + i]), verleak_bits);
        XORBit(i, GetLSB(commit_snd->commit_shares1[thread_params->out_lsb_blind_start + i]), verleak_bits);
      }

      for (int i = 0; i < AES_BITS; ++i) {
        std::copy(commit_snd->commit_shares0[thread_params->lsb_blind_start + i], commit_snd->commit_shares0[thread_params->lsb_blind_start + i] + CODEWORD_BYTES, verleak_decommit_shares0.get() + (num_verleaks + i) * CODEWORD_BYTES);
        std::copy(commit_snd->commit_shares1[thread_params->lsb_blind_start + i], commit_snd->commit_shares1[thread_params->lsb_blind_start + i] + CODEWORD_BYTES, verleak_decommit_shares1 + (num_verleaks + i) * CODEWORD_BYTES);
        SetBit(num_verleaks + i, GetLSB(commit_snd->commit_shares0[thread_params->lsb_blind_start + i]), verleak_bits);
        XORBit(num_verleaks + i, GetLSB(commit_snd->commit_shares1[thread_params->lsb_blind_start + i]), verleak_bits);
      }

      thread_params->chan.Send(verleak_bits, BITS_TO_BYTES(num_verleaks + AES_BITS));
      BatchDecommitLSB(commit_snd, verleak_decommit_shares0.get(), verleak_decommit_shares1, num_verleaks + AES_BITS);
      auto verleak_end = GET_TIME();
      durations[CONST_VERLEAK_TIME][exec_id] = verleak_end - verleak_begin;
      //===========================Run Garbling================================
      auto garbling_begin = GET_TIME();

      //Holds all memory needed for garbling
      std::unique_ptr<uint8_t[]> raw_garbling_data(std::make_unique<uint8_t[]>(3 * thread_params->Q * CSEC_BYTES + 2 * thread_params->A * CSEC_BYTES + (3 * thread_params->Q + thread_params->A) * CSEC_BYTES));
      std::unique_ptr<uint32_t[]> raw_id_data(std::make_unique<uint32_t[]>(thread_params->Q + thread_params->A));

      //For convenience we assign pointers into the garbling data.
      HalfGates gates_data;
      gates_data.T_G = raw_garbling_data.get();
      gates_data.T_E = gates_data.T_G + thread_params->Q * CSEC_BYTES;
      gates_data.S_O = gates_data.T_E + thread_params->Q * CSEC_BYTES;

      Auths auths_data;
      auths_data.H_0 = gates_data.S_O + thread_params->Q * CSEC_BYTES;
      auths_data.H_1 = auths_data.H_0 + thread_params->A * CSEC_BYTES;

      uint8_t* keys = auths_data.H_1 + thread_params->A * CSEC_BYTES;

      uint32_t* gate_ids = raw_id_data.get();
      uint32_t* auth_ids = gate_ids + thread_params->Q;

      //Construct all 0-keys used in gates and all gate ids
      for (int i = 0; i < thread_params->Q; ++i) {
        XOR_128(keys + i * CSEC_BYTES, commit_snd->commit_shares0[thread_params->left_keys_start + i], commit_snd->commit_shares1[thread_params->left_keys_start + i]);
        XOR_128(keys + (thread_params->Q + i) * CSEC_BYTES, commit_snd->commit_shares0[thread_params->right_keys_start + i], commit_snd->commit_shares1[thread_params->right_keys_start + i]);
        XOR_128(keys + (2 * thread_params->Q + i) * CSEC_BYTES, commit_snd->commit_shares0[thread_params->out_keys_start + i], commit_snd->commit_shares1[thread_params->out_keys_start + i]);
        gate_ids[i] = exec_id * (thread_params->Q + thread_params->A) + thread_params->out_keys_start + i;
      }

      //Garble all gates which stores the garbled tables in gates_data.T_G and gates_data.T_E and output keys in gates_data.S_O for convenience
      GarblingHandler gh(*thread_params);
      gh.GarbleGates(gates_data, 0, keys, keys + thread_params->Q * CSEC_BYTES, global_delta, gate_ids, thread_params->Q);

      //Solder output wire with the designated committed value for output wires
      for (uint32_t i = 0; i < thread_params->Q; ++i) {
        XOR_128(gates_data.S_O + i * CSEC_BYTES, keys + (2 * thread_params->Q + i) * CSEC_BYTES);
      }

      //Construct all 0-keys used for authenticators and all auth ids
      for (int i = 0; i < thread_params->A; ++i) {
        XOR_128(keys + (3 * thread_params->Q + i) * CSEC_BYTES, commit_snd->commit_shares0[thread_params->auth_start + i], commit_snd->commit_shares1[thread_params->auth_start + i]);

        auth_ids[i] = exec_id * (thread_params->Q + thread_params->A) + thread_params->auth_start + i;
      }

      //Garble the auths which stores the two authenticators in auths_data.H_0 and auths_data.H_1
      gh.GarbleAuths(auths_data, 0, keys + 3 * thread_params->Q * CSEC_BYTES, global_delta, auth_ids, thread_params->A);

      //Sends gates and auths (but not keys)
      thread_params->chan.Send(raw_garbling_data.get(), 3 * thread_params->Q * CSEC_BYTES + 2 * thread_params->A * CSEC_BYTES);

      auto garbling_end = GET_TIME();
      durations[CONST_GARBLING_TIME][exec_id] = garbling_end - garbling_begin;
      //========================Run Cut-and-Choose=============================

      //Receive challenge seed and sample check gates and check auths along with the challenge inputs to these. SampleChallenges populates all these variables
      // uint8_t* cnc_seed = thread_params->chan.blocking_receive();
      uint8_t cnc_seed[CSEC_BYTES];
      thread_params->chan.ReceiveBlocking(cnc_seed, CSEC_BYTES);
      auto cnc_begin = GET_TIME();

      //Sample check gates and check auths along with the challenge inputs to these. SampleChallenges populates all these variables
      int num_bytes_gates = BITS_TO_BYTES(thread_params->Q);
      int num_bytes_auths = BITS_TO_BYTES(thread_params->A);
      PRNG cnc_rand;
      cnc_rand.SetSeed(cnc_seed);

      std::unique_ptr<uint8_t[]> cnc_check_gates(std::make_unique<uint8_t[]>(num_bytes_gates + num_bytes_auths));
      uint8_t* cnc_check_auths = cnc_check_gates.get() + num_bytes_gates;
      WeightedRandomString(cnc_check_gates.get(), thread_params->p_g, num_bytes_gates, cnc_rand);
      WeightedRandomString(cnc_check_auths, thread_params->p_a, num_bytes_auths, cnc_rand);

      int num_check_gates = countSetBits(cnc_check_gates.get(), 0, thread_params->Q - 1);
      int num_check_auths = countSetBits(cnc_check_auths, 0, thread_params->A - 1);

      std::unique_ptr<uint8_t[]> left_cnc_input(std::make_unique<uint8_t[]>(3 * BITS_TO_BYTES(num_check_gates) + BITS_TO_BYTES(num_check_auths)));
      uint8_t* right_cnc_input = left_cnc_input.get() + BITS_TO_BYTES(num_check_gates);
      uint8_t* out_cnc_input = right_cnc_input + BITS_TO_BYTES(num_check_gates);
      uint8_t* auth_cnc_input = out_cnc_input + BITS_TO_BYTES(num_check_gates);

      cnc_rand.GenRnd(left_cnc_input.get(), BITS_TO_BYTES(num_check_gates));
      cnc_rand.GenRnd(right_cnc_input, BITS_TO_BYTES(num_check_gates));
      for (int i = 0; i < BITS_TO_BYTES(num_check_gates); ++i) {
        out_cnc_input[i] = left_cnc_input[i] & right_cnc_input[i];
      }

      cnc_rand.GenRnd(auth_cnc_input, BITS_TO_BYTES(num_check_auths));

      //Construct the CNC keys using the above-sampled information and also stores the check indices to be used for later decommit construction. Notice we only compute left and right keys as the output key can be computed on the evaluator side given these two. However we need to include the output key in the indices as they need to be included in the decommits.
      int num_checks = 3 * num_check_gates + num_check_auths;
      int num_check_keys_sent = 2 * num_check_gates + num_check_auths;
      std::unique_ptr<uint8_t[]> cnc_reply_keys(std::make_unique<uint8_t[]>(num_check_keys_sent * CSEC_BYTES));
      std::unique_ptr<uint8_t[]> cnc_decommit_shares0(std::make_unique<uint8_t[]>(2 * num_checks * CODEWORD_BYTES));
      uint8_t* cnc_decommit_shares1 = cnc_decommit_shares0.get() + num_checks * CODEWORD_BYTES;

      int current_auth_check_num = 0;
      int current_eval_auth_num = 0;
      for (uint32_t i = 0; i < thread_params->A; ++i) {
        if (GetBit(i, cnc_check_auths)) {
          XOR_128(cnc_reply_keys.get() + current_auth_check_num * CSEC_BYTES, commit_snd->commit_shares0[thread_params->auth_start + i], commit_snd->commit_shares1[thread_params->auth_start + i]);
          std::copy(commit_snd->commit_shares0[thread_params->auth_start + i], commit_snd->commit_shares0[thread_params->auth_start + i] + CODEWORD_BYTES, cnc_decommit_shares0.get() + current_auth_check_num * CODEWORD_BYTES);
          std::copy(commit_snd->commit_shares1[thread_params->auth_start + i], commit_snd->commit_shares1[thread_params->auth_start + i] + CODEWORD_BYTES, cnc_decommit_shares1 + current_auth_check_num * CODEWORD_BYTES);
          if (GetBit(current_auth_check_num, auth_cnc_input)) {
            XOR_128(cnc_reply_keys.get() + current_auth_check_num * CSEC_BYTES, global_delta);
            XOR_CodeWords(cnc_decommit_shares0.get() + current_auth_check_num * CODEWORD_BYTES, commit_snd->commit_shares0[thread_params->delta_pos]);
            XOR_CodeWords(cnc_decommit_shares1 + current_auth_check_num * CODEWORD_BYTES, commit_snd->commit_shares1[thread_params->delta_pos]);
          }
          ++current_auth_check_num;
        } else if (current_eval_auth_num < thread_params->num_eval_auths) {
          //Populate the array with the correct eval auth indices
          tmp_auth_eval_ids[thread_params->num_eval_auths * exec_id + current_eval_auth_num] = exec_id * (thread_params->Q + thread_params->A) + thread_params->auth_start + i;
          ++current_eval_auth_num;
        }
      }

      //Now for the gates
      int current_check_num = 0;
      int current_eval_gate_num = 0;
      for (uint32_t i = 0; i < thread_params->Q; ++i) {
        if (GetBit(i, cnc_check_gates.get())) {

          //Left
          XOR_128(cnc_reply_keys.get() + (num_check_auths + current_check_num) * CSEC_BYTES, commit_snd->commit_shares0[thread_params->left_keys_start + i], commit_snd->commit_shares1[thread_params->left_keys_start + i]);
          std::copy(commit_snd->commit_shares0[thread_params->left_keys_start + i], commit_snd->commit_shares0[thread_params->left_keys_start + i] + CODEWORD_BYTES, cnc_decommit_shares0.get() + (num_check_auths + current_check_num) * CODEWORD_BYTES);
          std::copy(commit_snd->commit_shares1[thread_params->left_keys_start + i], commit_snd->commit_shares1[thread_params->left_keys_start + i] + CODEWORD_BYTES, cnc_decommit_shares1 + (num_check_auths + current_check_num) * CODEWORD_BYTES);

          //We include the global delta if the left-input is supposed to be 1.
          if (GetBit(current_check_num, left_cnc_input.get())) {
            XOR_128(cnc_reply_keys.get() + (num_check_auths + current_check_num) * CSEC_BYTES, global_delta);
            XOR_CodeWords(cnc_decommit_shares0.get() + (num_check_auths + current_check_num) * CODEWORD_BYTES, commit_snd->commit_shares0[thread_params->delta_pos]);
            XOR_CodeWords(cnc_decommit_shares1 + (num_check_auths + current_check_num) * CODEWORD_BYTES, commit_snd->commit_shares1[thread_params->delta_pos]);
          }

          //Right
          XOR_128(cnc_reply_keys.get() + (num_check_auths + num_check_gates + current_check_num) * CSEC_BYTES, commit_snd->commit_shares0[thread_params->right_keys_start + i], commit_snd->commit_shares1[thread_params->right_keys_start + i]);
          std::copy(commit_snd->commit_shares0[thread_params->right_keys_start + i], commit_snd->commit_shares0[thread_params->right_keys_start + i] + CODEWORD_BYTES, cnc_decommit_shares0.get() + (num_check_auths + num_check_gates + current_check_num) * CODEWORD_BYTES);
          std::copy(commit_snd->commit_shares1[thread_params->right_keys_start + i], commit_snd->commit_shares1[thread_params->right_keys_start + i] + CODEWORD_BYTES, cnc_decommit_shares1 + (num_check_auths + num_check_gates + current_check_num) * CODEWORD_BYTES);

          //We include the global delta if the right-input is supposed to be 1.
          if (GetBit(current_check_num, right_cnc_input)) {
            XOR_128(cnc_reply_keys.get() + (num_check_auths + num_check_gates + current_check_num) * CSEC_BYTES, global_delta);
            XOR_CodeWords(cnc_decommit_shares0.get() + (num_check_auths + num_check_gates + current_check_num) * CODEWORD_BYTES, commit_snd->commit_shares0[thread_params->delta_pos]);
            XOR_CodeWords(cnc_decommit_shares1 + (num_check_auths + num_check_gates + current_check_num) * CODEWORD_BYTES, commit_snd->commit_shares1[thread_params->delta_pos]);
          }

          //Out
          std::copy(commit_snd->commit_shares0[thread_params->out_keys_start + i], commit_snd->commit_shares0[thread_params->out_keys_start + i] + CODEWORD_BYTES, cnc_decommit_shares0.get() + (num_check_auths + 2 * num_check_gates + current_check_num) * CODEWORD_BYTES);
          std::copy(commit_snd->commit_shares1[thread_params->out_keys_start + i], commit_snd->commit_shares1[thread_params->out_keys_start + i] + CODEWORD_BYTES, cnc_decommit_shares1 + (num_check_auths + 2 * num_check_gates + current_check_num) * CODEWORD_BYTES);

          //We include the global delta if the right-input is supposed to be 1.
          if (GetBit(current_check_num, out_cnc_input)) {
            XOR_CodeWords(cnc_decommit_shares0.get() + (num_check_auths + 2 * num_check_gates + current_check_num) * CODEWORD_BYTES, commit_snd->commit_shares0[thread_params->delta_pos]);
            XOR_CodeWords(cnc_decommit_shares1 + (num_check_auths + 2 * num_check_gates + current_check_num) * CODEWORD_BYTES, commit_snd->commit_shares1[thread_params->delta_pos]);
          }
          ++current_check_num;
        }
        else if (current_eval_gate_num < thread_params->num_eval_gates) {
          //Populate the array with the correct eval gate indices
          tmp_gate_eval_ids[thread_params->num_eval_gates * exec_id + current_eval_gate_num] = exec_id * (thread_params->Q + thread_params->A) + thread_params->out_keys_start + i;
          ++current_eval_gate_num;
        }
      }

      //Send all challenge keys
      thread_params->chan.Send(cnc_reply_keys.get(), num_check_keys_sent * CSEC_BYTES);

      //Start decommit phase using the above-created indices
      commit_snd->BatchDecommit(cnc_decommit_shares0.get(), cnc_decommit_shares1, num_checks);
      auto cnc_end = GET_TIME();
      durations[CONST_CNC_TIME][exec_id] = cnc_end - cnc_begin;
    });
  }

  //Wait for all CNC executions to finish
  for (std::future<void>& r : cnc_execs_finished) {
    r.wait();
  }

  //Receive bucketing info
  uint8_t bucket_seed[CSEC];
  params.chan.ReceiveBlocking(bucket_seed, CSEC_BYTES);
  PRNG bucket_rnd;
  bucket_rnd.SetSeed(bucket_seed);
  uint8_t bucket_seeds[2 * CSEC_BYTES];
  bucket_rnd.GenRnd(bucket_seeds, 2 * CSEC_BYTES);

  std::unique_ptr<uint32_t[]> permuted_eval_ids_ptr(new uint32_t[params.num_eval_gates + params.num_eval_auths]);
  uint32_t* permuted_eval_gates_ids = permuted_eval_ids_ptr.get();
  uint32_t* permuted_eval_auths_ids = permuted_eval_gates_ids + params.num_eval_gates;

  //Initialize the permutation arrays
  for (uint32_t i = 0; i < params.num_eval_gates; ++i) {
    permuted_eval_gates_ids[i] = i;
  }
  for (uint32_t i = 0; i < params.num_eval_auths; ++i) {
    permuted_eval_auths_ids[i] = i;
  }
  PermuteArray(permuted_eval_gates_ids, params.num_eval_gates, bucket_seeds);
  PermuteArray(permuted_eval_auths_ids, params.num_eval_auths, bucket_seeds + CSEC_BYTES);

  for (uint32_t i = 0; i < params.num_eval_gates; ++i) {
    eval_gates_ids[permuted_eval_gates_ids[i]] = tmp_gate_eval_ids[i];
  }

  for (uint32_t i = 0; i < params.num_eval_auths; ++i) {
    eval_auths_ids[permuted_eval_auths_ids[i]] = tmp_auth_eval_ids[i];
  }

  //Setup maps from eval_gates and eval_auths to commit_block and inner block commit index. Needed to construct decommits that span all executions
  IDMap eval_gates_to_blocks(eval_gates_ids, thread_params_vec[0]->Q + thread_params_vec[0]->A, thread_params_vec[0]->out_keys_start);
  IDMap eval_auths_to_blocks(eval_auths_ids, thread_params_vec[0]->Q + thread_params_vec[0]->A, thread_params_vec[0]->auth_start);

  //Starts params.num_execs parallel executions for preprocessing solderings. We reuse much of the execution specific information from the last parallel executions
  auto presolder_begin = GET_TIME();
  std::vector<std::future<void>> pre_soldering_execs_finished(params.num_execs);
  for (int exec_id = 0; exec_id < params.num_execs; ++exec_id) {
    int inp_from = inputs_from[exec_id];
    int inp_to = inputs_to[exec_id];
    int ga_inp_from = gates_inputs_from[exec_id];
    int ga_inp_to = gates_inputs_to[exec_id];
    int ga_from = gates_from[exec_id];
    int ga_to = gates_to[exec_id];
    Params* thread_params = thread_params_vec[exec_id].get();

    pre_soldering_execs_finished[exec_id] = thread_pool.push([this, thread_params, exec_id, inp_from, inp_to, ga_inp_from, ga_inp_to, ga_from, ga_to, &eval_gates_to_blocks, &eval_auths_to_blocks] (int id) {

      int num_gates = thread_params->num_pre_gates;
      int num_inputs = thread_params->num_pre_inputs;

      //Set some often used variables
      int num_gate_solderings = num_gates * (params.num_bucket - 1) + (num_inputs / 2) * (params.num_inp_bucket - 1);
      // int num_inp_gate_solderings =  ;
      int num_auth_solderings = num_gates * params.num_auth;
      int num_inp_auth_solderings = num_inputs * (params.num_inp_auth - 1);
      int num_pre_solderings = 3 * num_gate_solderings + num_auth_solderings + num_inp_auth_solderings;

      //Create raw preprocessed solderings data and point into this for convenience
      std::unique_ptr<uint8_t[]> pre_solderings(std::make_unique<uint8_t[]>(num_pre_solderings * CSEC_BYTES + 3 * CSEC_BYTES + 2 * (num_pre_solderings * CODEWORD_BYTES)));

      uint8_t* left_wire_solderings = pre_solderings.get();
      uint8_t* right_wire_solderings = left_wire_solderings + CSEC_BYTES * num_gate_solderings;
      uint8_t* out_wire_solderings = right_wire_solderings + CSEC_BYTES * num_gate_solderings;
      uint8_t* bucket_auth_solderings = out_wire_solderings + CSEC_BYTES * num_gate_solderings;
      uint8_t* input_auth_solderings = bucket_auth_solderings + CSEC_BYTES * num_auth_solderings;

      uint8_t* current_head_keys0 = input_auth_solderings + CSEC_BYTES * num_inp_auth_solderings;
      uint8_t* current_head_keys1 = current_head_keys0 + CSEC_BYTES;
      uint8_t* current_head_keys2 = current_head_keys1 + CSEC_BYTES;
      uint8_t* presolder_decommit_shares0 = current_head_keys2 + CSEC_BYTES;
      uint8_t* presolder_decommit_shares1 = presolder_decommit_shares0 + num_pre_solderings * CODEWORD_BYTES;

      // Construct the actual solderings
      int curr_head_gate_pos, curr_head_block, curr_head_idx, curr_gate_pos, curr_gate_block, curr_gate_idx, curr_auth_pos, curr_auth_block, curr_auth_idx, curr_head_inp_auth_pos, curr_head_inp_auth_block, curr_head_inp_auth_idx, curr_inp_auth_pos;

      int solder_gate_pos = 0;
      int solder_auth_pos = 0;
      int solder_inp_auth_pos = 0;

      //We first loop over all head gates
      for (int i = ga_from; i < ga_to; ++i) {
        curr_head_gate_pos = i * params.num_bucket;
        eval_gates_to_blocks.GetExecIDAndIndex(curr_head_gate_pos, curr_head_block, curr_head_idx);

        XOR_128(current_head_keys0, commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx]);

        XOR_128(current_head_keys1, commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx]);

        XOR_128(current_head_keys2, commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx]);

        //Then all of the gates in this head_gate's bucket (notice j starts at 1)
        for (int j = 1; j < params.num_bucket; ++j) {
          curr_gate_pos = curr_head_gate_pos + j;
          eval_gates_to_blocks.GetExecIDAndIndex(curr_gate_pos, curr_gate_block, curr_gate_idx);

          //Left soldering
          XOR_128(left_wire_solderings + solder_gate_pos * CSEC_BYTES, commit_snds[curr_gate_block]->commit_shares0[thread_params->left_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->left_keys_start + curr_gate_idx]);
          XOR_128(left_wire_solderings + solder_gate_pos * CSEC_BYTES, current_head_keys0);

          //Decommit shares
          std::copy(commit_snds[curr_gate_block]->commit_shares0[thread_params->left_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares0[thread_params->left_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + solder_gate_pos * CODEWORD_BYTES);
          std::copy(commit_snds[curr_gate_block]->commit_shares1[thread_params->left_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->left_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + solder_gate_pos * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + solder_gate_pos * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + solder_gate_pos * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx]);

          //Right soldering
          XOR_128(right_wire_solderings + solder_gate_pos * CSEC_BYTES, commit_snds[curr_gate_block]->commit_shares0[thread_params->right_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->right_keys_start + curr_gate_idx]);
          XOR_128(right_wire_solderings + solder_gate_pos * CSEC_BYTES, current_head_keys1);

          //Decommit shares
          std::copy(commit_snds[curr_gate_block]->commit_shares0[thread_params->right_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares0[thread_params->right_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);
          std::copy(commit_snds[curr_gate_block]->commit_shares1[thread_params->right_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->right_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx]);

          //Out soldering
          XOR_128(out_wire_solderings + solder_gate_pos * CSEC_BYTES, commit_snds[curr_gate_block]->commit_shares0[thread_params->out_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->out_keys_start + curr_gate_idx]);
          XOR_128(out_wire_solderings + solder_gate_pos * CSEC_BYTES, current_head_keys2);

          //Decommit shares
          std::copy(commit_snds[curr_gate_block]->commit_shares0[thread_params->out_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares0[thread_params->out_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);
          std::copy(commit_snds[curr_gate_block]->commit_shares1[thread_params->out_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->out_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx]);

          ++solder_gate_pos;
        }

        //Then all of the authenticators attached to this head_gate. Here j starts at 0 since all auths are attached to head_gate's output wire
        for (int j = 0; j < params.num_auth; ++j) {
          curr_auth_pos = i * params.num_auth + j;
          eval_auths_to_blocks.GetExecIDAndIndex(curr_auth_pos, curr_auth_block, curr_auth_idx);

          //Bucket auth soldering
          XOR_128(bucket_auth_solderings + solder_auth_pos * CSEC_BYTES, commit_snds[curr_auth_block]->commit_shares0[thread_params->auth_start + curr_auth_idx], commit_snds[curr_auth_block]->commit_shares1[thread_params->auth_start + curr_auth_idx]);

          XOR_128(bucket_auth_solderings + solder_auth_pos * CSEC_BYTES, current_head_keys2);

          //Decommit shares
          std::copy(commit_snds[curr_auth_block]->commit_shares0[thread_params->auth_start + curr_auth_idx], commit_snds[curr_auth_block]->commit_shares0[thread_params->auth_start + curr_auth_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + (3 * num_gate_solderings + solder_auth_pos) * CODEWORD_BYTES);
          std::copy(commit_snds[curr_auth_block]->commit_shares1[thread_params->auth_start + curr_auth_idx], commit_snds[curr_auth_block]->commit_shares1[thread_params->auth_start + curr_auth_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + (3 * num_gate_solderings + solder_auth_pos) * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + (3 * num_gate_solderings + solder_auth_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + (3 * num_gate_solderings + solder_auth_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx]);

          ++solder_auth_pos;
        }
      }

      for (int i = ga_inp_from; i < ga_inp_to; ++i) {
        curr_head_gate_pos = params.num_pre_gates * params.num_bucket + i * params.num_inp_bucket;
        eval_gates_to_blocks.GetExecIDAndIndex(curr_head_gate_pos, curr_head_block, curr_head_idx);

        XOR_128(current_head_keys0, commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx]);

        XOR_128(current_head_keys1, commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx]);

        XOR_128(current_head_keys2, commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx]);

        //Then all of the gates in this head_gate's bucket (notice j starts at 1)
        for (int j = 1; j < params.num_inp_bucket; ++j) {
          curr_gate_pos = curr_head_gate_pos + j;
          eval_gates_to_blocks.GetExecIDAndIndex(curr_gate_pos, curr_gate_block, curr_gate_idx);

          //Left soldering
          XOR_128(left_wire_solderings + solder_gate_pos * CSEC_BYTES, commit_snds[curr_gate_block]->commit_shares0[thread_params->left_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->left_keys_start + curr_gate_idx]);
          XOR_128(left_wire_solderings + solder_gate_pos * CSEC_BYTES, current_head_keys0);

          //Decommit shares
          std::copy(commit_snds[curr_gate_block]->commit_shares0[thread_params->left_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares0[thread_params->left_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + solder_gate_pos * CODEWORD_BYTES);
          std::copy(commit_snds[curr_gate_block]->commit_shares1[thread_params->left_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->left_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + solder_gate_pos * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + solder_gate_pos * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + solder_gate_pos * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx]);

          //Right soldering
          XOR_128(right_wire_solderings + solder_gate_pos * CSEC_BYTES, commit_snds[curr_gate_block]->commit_shares0[thread_params->right_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->right_keys_start + curr_gate_idx]);
          XOR_128(right_wire_solderings + solder_gate_pos * CSEC_BYTES, current_head_keys1);

          //Decommit shares
          std::copy(commit_snds[curr_gate_block]->commit_shares0[thread_params->right_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares0[thread_params->right_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);
          std::copy(commit_snds[curr_gate_block]->commit_shares1[thread_params->right_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->right_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + (num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx]);

          //Out soldering
          XOR_128(out_wire_solderings + solder_gate_pos * CSEC_BYTES, commit_snds[curr_gate_block]->commit_shares0[thread_params->out_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->out_keys_start + curr_gate_idx]);
          XOR_128(out_wire_solderings + solder_gate_pos * CSEC_BYTES, current_head_keys2);

          //Decommit shares
          std::copy(commit_snds[curr_gate_block]->commit_shares0[thread_params->out_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares0[thread_params->out_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);
          std::copy(commit_snds[curr_gate_block]->commit_shares1[thread_params->out_keys_start + curr_gate_idx], commit_snds[curr_gate_block]->commit_shares1[thread_params->out_keys_start + curr_gate_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + (2 * num_gate_solderings + solder_gate_pos) * CODEWORD_BYTES, commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx]);

          ++solder_gate_pos;
        }
      }

      //Finally we create the solderings for input authentication. This is constructed exactly the same as the bucket_solderings as the principle is the same using a head input auth and then solderings onto this all the num_inp_auth-1 other authenticators.
      for (int i = inp_from; i < inp_to; ++i) {
        curr_head_inp_auth_pos = params.num_pre_gates * params.num_auth + i * params.num_inp_auth;
        eval_auths_to_blocks.GetExecIDAndIndex(curr_head_inp_auth_pos, curr_head_inp_auth_block, curr_head_inp_auth_idx);

        XOR_128(current_head_keys0, commit_snds[curr_head_inp_auth_block]->commit_shares0[thread_params->auth_start + curr_head_inp_auth_idx], commit_snds[curr_head_inp_auth_block]->commit_shares1[thread_params->auth_start + curr_head_inp_auth_idx]);

        for (int j = 1; j < params.num_inp_auth; ++j) {
          curr_inp_auth_pos = curr_head_inp_auth_pos + j;
          eval_auths_to_blocks.GetExecIDAndIndex(curr_inp_auth_pos, curr_auth_block, curr_auth_idx);

          //Inp auth soldering
          XOR_128(input_auth_solderings + solder_inp_auth_pos * CSEC_BYTES, commit_snds[curr_auth_block]->commit_shares0[thread_params->auth_start + curr_auth_idx], commit_snds[curr_auth_block]->commit_shares1[thread_params->auth_start + curr_auth_idx]);
          XOR_128(input_auth_solderings + solder_inp_auth_pos * CSEC_BYTES, current_head_keys0);

          //Decommit shares
          std::copy(commit_snds[curr_auth_block]->commit_shares0[thread_params->auth_start + curr_auth_idx], commit_snds[curr_auth_block]->commit_shares0[thread_params->auth_start + curr_auth_idx] + CODEWORD_BYTES, presolder_decommit_shares0 + (3 * num_gate_solderings + num_auth_solderings + solder_inp_auth_pos) * CODEWORD_BYTES);
          std::copy(commit_snds[curr_auth_block]->commit_shares1[thread_params->auth_start + curr_auth_idx], commit_snds[curr_auth_block]->commit_shares1[thread_params->auth_start + curr_auth_idx] + CODEWORD_BYTES, presolder_decommit_shares1 + (3 * num_gate_solderings + num_auth_solderings + solder_inp_auth_pos) * CODEWORD_BYTES);

          XOR_CodeWords(presolder_decommit_shares0 + (3 * num_gate_solderings + num_auth_solderings + solder_inp_auth_pos) * CODEWORD_BYTES, commit_snds[curr_head_inp_auth_block]->commit_shares0[thread_params->auth_start + curr_head_inp_auth_idx]);
          XOR_CodeWords(presolder_decommit_shares1 + (3 * num_gate_solderings + num_auth_solderings + solder_inp_auth_pos) * CODEWORD_BYTES, commit_snds[curr_head_inp_auth_block]->commit_shares1[thread_params->auth_start + curr_head_inp_auth_idx]);

          ++solder_inp_auth_pos;
        }
      }

      //We end by sending the produced solderings and starting the batch decommit procedure which uses commitments from all executions to build the decommits
      thread_params->chan.Send(pre_solderings.get(), CSEC_BYTES * num_pre_solderings);
      commit_snds[exec_id]->BatchDecommit(presolder_decommit_shares0, presolder_decommit_shares1, num_pre_solderings);

    });
  }

  //Wait for all preprocessed soldering executions to finish
  for (std::future<void>& r : pre_soldering_execs_finished) {
    r.wait();
  }

  auto presolder_end = GET_TIME();

///////////////// //DEBUG for testing correctness of solderings////////////////
#ifdef DEBUG_SOLDERINGS_INP_BUCKETS
  std::unique_ptr<uint8_t[]> keys_ptr(std::make_unique<uint8_t[]>(CSEC_BYTES * (3 * (params.num_pre_gates + params.num_pre_inputs / 2) + params.num_pre_inputs)));
  uint8_t* keys = keys_ptr.get();

  for (int i = 0; i < params.num_pre_gates; ++i) {
    int curr_head_pos = i * params.num_bucket;
    int curr_head_block, curr_head_idx;
    eval_gates_to_blocks.GetExecIDAndIndex(curr_head_pos, curr_head_block, curr_head_idx);
    XOR_128(keys + i * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params_vec[0]->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params_vec[0]->left_keys_start + curr_head_idx]);

    XOR_128(keys + (params.num_pre_gates + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params_vec[0]->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params_vec[0]->right_keys_start + curr_head_idx]);
    XOR_128(keys + (2 * params.num_pre_gates + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params_vec[0]->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params_vec[0]->out_keys_start + curr_head_idx]);
  }

  for (int i = 0; i < params.num_pre_inputs / 2; ++i) {
    int curr_head_pos = params.num_pre_gates * params.num_bucket + i * params.num_inp_bucket;
    int curr_head_block, curr_head_idx;
    eval_gates_to_blocks.GetExecIDAndIndex(curr_head_pos, curr_head_block, curr_head_idx);
    XOR_128(keys + (3 * params.num_pre_gates + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params_vec[0]->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params_vec[0]->left_keys_start + curr_head_idx]);

    XOR_128(keys + (3 * params.num_pre_gates + params.num_pre_inputs / 2 + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params_vec[0]->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params_vec[0]->right_keys_start + curr_head_idx]);
    XOR_128(keys + (3 * params.num_pre_gates + 2 * params.num_pre_inputs / 2 + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params_vec[0]->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params_vec[0]->out_keys_start + curr_head_idx]);
  }

  for (int i = 0; i < params.num_pre_inputs; ++i) {
    int curr_auth_inp_head_pos = params.num_pre_gates * params.num_auth + i * params.num_inp_auth;
    int curr_inp_head_block, curr_inp_head_idx;
    eval_auths_to_blocks.GetExecIDAndIndex(curr_auth_inp_head_pos, curr_inp_head_block, curr_inp_head_idx);

    XOR_128(keys + (3 * params.num_pre_gates + 3 * params.num_pre_inputs / 2 + i) * CSEC_BYTES, commit_snds[curr_inp_head_block]->commit_shares0[thread_params_vec[0]->auth_start + curr_inp_head_idx], commit_snds[curr_inp_head_block]->commit_shares1[thread_params_vec[0]->auth_start + curr_inp_head_idx]);
  }

  params.chan.Send(keys, CSEC_BYTES * (3 * (params.num_pre_gates + params.num_pre_inputs / 2) + params.num_pre_inputs));
#endif
///////////////// //DEBUG for testing correctness of solderings////////////////

  auto setup_end = GET_TIME();

  uint64_t bytes_received = params.chan.GetTotalBytesReceived();
  for (std::unique_ptr<Params>& thread_params : thread_params_vec) {
    bytes_received += thread_params->chan.GetTotalBytesReceived();
  }

  std::vector<std::chrono::duration<long double, std::milli>> durations_res(EVAL_NUM_TIMINGS, std::chrono::duration<long double, std::milli>(0));

  for (int i = 0; i < EVAL_NUM_TIMINGS; ++i) {
    for (int j = 0; j < params.num_execs; ++j)
    {
      durations_res[i] += durations[i][j];
    }
    durations_res[i] = durations_res[i] / params.num_execs;
  }

#ifdef TINY_PRINT
  std::cout << "Avg. Commit: " << durations_res[CONST_COMMIT_TIME].count() << std::endl;
  std::cout << "Avg. Verleak: " << durations_res[CONST_VERLEAK_TIME].count() << std::endl;
  std::cout << "Avg. Garbling: " << durations_res[CONST_GARBLING_TIME].count() << std::endl;
  std::cout << "Avg. CnC: " << durations_res[CONST_CNC_TIME].count() << std::endl;
  PRINT_TIME(presolder_end, presolder_begin, "PRE_SOLDER");
  PRINT_TIME(setup_end, setup_begin, "SETUP_TOTAL");
  std::cout << "Received " << bytes_received / 1000000 << " MB" << std::endl;
#endif
}

void TinyConstructor::Offline(std::vector<Circuit*>& circuits, int top_num_execs) {
  // int top_num_execs = std::min((int)circuits.size(), offline_num_execs);
  std::vector<std::future<void>> top_soldering_execs_finished(top_num_execs);
  //Split the number of preprocessed gates and inputs into top_num_execs executions
  std::vector<int> circuits_from, circuits_to;
  PartitionBufferFixedNum(circuits_from, circuits_to, top_num_execs, circuits.size());

  int num_gates_needed = 0;
  int num_inp_gates_needed = 0;
  int num_inps_needed = 0;
  int num_outs_needed = 0;
  for (int i = 0; i < circuits.size(); ++i) {
    gates_offset.emplace_back(num_gates_used + num_gates_needed);
    inp_gates_offset.emplace_back(num_inputs_used / 2 + num_inp_gates_needed);
    inputs_offset.emplace_back(num_inputs_used + num_inps_needed);
    outputs_offset.emplace_back(num_outputs_used + num_outs_needed);
    num_gates_needed += circuits[i]->num_and_gates;
    num_inp_gates_needed += circuits[i]->num_const_inp_wires / 2;
    num_inps_needed += circuits[i]->num_inp_wires;
    num_outs_needed += circuits[i]->num_out_wires;
  }

  if ((params.num_pre_gates - num_gates_used) < num_gates_needed) {
    throw std::runtime_error("Not enough garbled gates");
  } else {
    num_gates_used += num_gates_needed;
  }

  //Due to the way we choose our parameters, if there are enough num_inps, then there are also enough for inp_gates.
  if ((params.num_pre_inputs - num_inputs_used) < num_inps_needed) {
    throw std::runtime_error("Not enough input authenticators");
  } else {
    num_inputs_used += num_inps_needed;
  }

  if ((params.num_pre_outputs - num_outputs_used) < num_outs_needed) {
    throw std::runtime_error("Not enough output wires");
  } else {
    num_outputs_used += num_outs_needed;
  }

  //Setup maps from eval_gates and eval_auths to commit_block and inner block commit index. Needed to construct decommits that span all executions
  IDMap eval_gates_to_blocks(eval_gates_ids, thread_params_vec[0]->Q + thread_params_vec[0]->A, thread_params_vec[0]->out_keys_start);
  IDMap eval_auths_to_blocks(eval_auths_ids, thread_params_vec[0]->Q + thread_params_vec[0]->A, thread_params_vec[0]->auth_start);

  auto top_soldering_begin = GET_TIME();
  for (int exec_id = 0; exec_id < top_num_execs; ++exec_id) {
    int circ_from = circuits_from[exec_id];
    int circ_to = circuits_to[exec_id];

    Params* thread_params = thread_params_vec[exec_id].get();

    top_soldering_execs_finished[exec_id] = thread_pool.push([this, thread_params, exec_id, circ_from, circ_to, &circuits, & eval_gates_to_blocks, &eval_auths_to_blocks] (int id) {


      for (int c = circ_from; c < circ_to; ++c) {
        Circuit* circuit = circuits[c];
        int gate_offset = gates_offset[c];
        int inp_gate_offset = inp_gates_offset[c];
        int inp_offset = inputs_offset[c];

        int num_top_solderings = 2 * circuit->num_and_gates + circuit->num_const_inp_wires; //the 2* factor cancels out as we can check two inputs pr. input bucket.

        std::unique_ptr<uint8_t[]> topological_solderings(std::make_unique<uint8_t[]>(num_top_solderings * (CSEC_BYTES + 2 * CODEWORD_BYTES) + circuit->num_wires * (2 * CODEWORD_BYTES + CSEC_BYTES)));
        uint8_t* topsolder_decommit_shares0 = topological_solderings.get() + num_top_solderings * CSEC_BYTES;
        uint8_t* topsolder_decommit_shares1 = topsolder_decommit_shares0 + num_top_solderings * CODEWORD_BYTES;
        uint8_t* decommit_shares_tmp0 = topsolder_decommit_shares1 + num_top_solderings * CODEWORD_BYTES;
        uint8_t* decommit_shares_tmp1 = decommit_shares_tmp0 + circuit->num_wires * CODEWORD_BYTES;
        uint8_t* values = decommit_shares_tmp1 + circuit->num_wires * CODEWORD_BYTES;

        int curr_auth_inp_head_pos, curr_inp_head_block, curr_inp_head_idx, curr_head_pos, curr_head_block, curr_head_idx;


        for (int i = 0; i < circuit->num_inp_wires; ++i) {
          curr_auth_inp_head_pos = params.num_pre_gates * thread_params->num_auth + (inp_offset + i) * thread_params->num_inp_auth;
          eval_auths_to_blocks.GetExecIDAndIndex(curr_auth_inp_head_pos, curr_inp_head_block, curr_inp_head_idx);

          std::copy(commit_snds[curr_inp_head_block]->commit_shares0[thread_params->auth_start + curr_inp_head_idx], commit_snds[curr_inp_head_block]->commit_shares0[thread_params->auth_start + curr_inp_head_idx] + CSEC_BYTES, values + i * CSEC_BYTES);
          XOR_128(values + i * CSEC_BYTES, commit_snds[curr_inp_head_block]->commit_shares1[thread_params->auth_start + curr_inp_head_idx]);

          //Build decommit_info
          std::copy(commit_snds[curr_inp_head_block]->commit_shares0[thread_params->auth_start + curr_inp_head_idx], commit_snds[curr_inp_head_block]->commit_shares0[thread_params->auth_start + curr_inp_head_idx] + CODEWORD_BYTES, decommit_shares_tmp0 + i * CODEWORD_BYTES);
          std::copy(commit_snds[curr_inp_head_block]->commit_shares1[thread_params->auth_start + curr_inp_head_idx], commit_snds[curr_inp_head_block]->commit_shares1[thread_params->auth_start + curr_inp_head_idx] + CODEWORD_BYTES, decommit_shares_tmp1 + i * CODEWORD_BYTES);
        }

        int left_inp_start = circuit->num_and_gates;
        int right_inp_start = 2 * circuit->num_and_gates + circuit->num_const_inp_wires / 2;
        for (int i = 0; i < circuit->num_const_inp_wires / 2; ++i) {
          curr_head_pos = params.num_pre_gates * params.num_bucket + (inp_gate_offset + i) * params.num_inp_bucket;
          eval_gates_to_blocks.GetExecIDAndIndex(curr_head_pos, curr_head_block, curr_head_idx);

          //Left
          XOR_128(topological_solderings.get() + (left_inp_start + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx]);

          XOR_128(topological_solderings.get() + (left_inp_start + i) * CSEC_BYTES, values + i * CSEC_BYTES);

          std::copy(commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares0 + (left_inp_start + i) * CODEWORD_BYTES);
          XOR_CodeWords(topsolder_decommit_shares0 + (left_inp_start + i) * CODEWORD_BYTES, decommit_shares_tmp0 + i * CODEWORD_BYTES);

          std::copy(commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares1 + (left_inp_start + i) * CODEWORD_BYTES);
          XOR_CodeWords(topsolder_decommit_shares1 + (left_inp_start + i) * CODEWORD_BYTES, decommit_shares_tmp1 + i * CODEWORD_BYTES);

          //Right
          XOR_128(topological_solderings.get() + (right_inp_start + i) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx]);

          XOR_128(topological_solderings.get() + (right_inp_start + i) * CSEC_BYTES, values + (circuit->num_const_inp_wires / 2 + i) * CSEC_BYTES);

          std::copy(commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares0 + (right_inp_start + i) * CODEWORD_BYTES);
          XOR_CodeWords(topsolder_decommit_shares0 + (right_inp_start + i) * CODEWORD_BYTES, decommit_shares_tmp0 + (circuit->num_const_inp_wires / 2 + i) * CODEWORD_BYTES);

          std::copy(commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares1 + (right_inp_start + i) * CODEWORD_BYTES);
          XOR_CodeWords(topsolder_decommit_shares1 + (right_inp_start + i) * CODEWORD_BYTES, decommit_shares_tmp1 + (circuit->num_const_inp_wires / 2 + i) * CODEWORD_BYTES);
        }

        int curr_and_gate = 0;
        int left_gate_start = 0;
        int right_gate_start = circuit->num_and_gates + circuit->num_const_inp_wires / 2;
        for (int i = 0; i < circuit->num_gates; ++i) {
          Gate g = circuit->gates[i];
          if (g.type == NOT) {
            std::copy(values + g.left_wire * CSEC_BYTES, values + g.left_wire * CSEC_BYTES + CSEC_BYTES, values + g.out_wire * CSEC_BYTES);
            XOR_128(values + g.out_wire * CSEC_BYTES, commit_snds[0]->commit_shares0[thread_params->delta_pos]);
            XOR_128(values + g.out_wire * CSEC_BYTES, commit_snds[0]->commit_shares1[thread_params->delta_pos]);

            //Build decommit_info
            std::copy(decommit_shares_tmp0 + g.left_wire * CODEWORD_BYTES, decommit_shares_tmp0 + g.left_wire * CODEWORD_BYTES + CODEWORD_BYTES, decommit_shares_tmp0 + g.out_wire * CODEWORD_BYTES);
            std::copy(decommit_shares_tmp1 + g.left_wire * CODEWORD_BYTES, decommit_shares_tmp1 + g.left_wire * CODEWORD_BYTES + CODEWORD_BYTES, decommit_shares_tmp1 + g.out_wire * CODEWORD_BYTES);

            XOR_CodeWords(decommit_shares_tmp0 + g.out_wire * CODEWORD_BYTES, commit_snds[0]->commit_shares0[thread_params->delta_pos]);
            XOR_CodeWords(decommit_shares_tmp1 + g.out_wire * CODEWORD_BYTES, commit_snds[0]->commit_shares1[thread_params->delta_pos]);

          } else if (g.type == XOR) {
            std::copy(values + g.left_wire * CSEC_BYTES, values + g.left_wire * CSEC_BYTES + CSEC_BYTES, values + g.out_wire * CSEC_BYTES);
            XOR_128(values + g.out_wire * CSEC_BYTES, values + g.right_wire * CSEC_BYTES);

            //Build decommit_info
            std::copy(decommit_shares_tmp0 + g.left_wire * CODEWORD_BYTES, decommit_shares_tmp0 + g.left_wire * CODEWORD_BYTES + CODEWORD_BYTES, decommit_shares_tmp0 + g.out_wire * CODEWORD_BYTES);
            std::copy(decommit_shares_tmp1 + g.left_wire * CODEWORD_BYTES, decommit_shares_tmp1 + g.left_wire * CODEWORD_BYTES + CODEWORD_BYTES, decommit_shares_tmp1 + g.out_wire * CODEWORD_BYTES);

            XOR_CodeWords(decommit_shares_tmp0 + g.out_wire * CODEWORD_BYTES, decommit_shares_tmp0 + g.right_wire * CODEWORD_BYTES);
            XOR_CodeWords(decommit_shares_tmp1 + g.out_wire * CODEWORD_BYTES, decommit_shares_tmp1 + g.right_wire * CODEWORD_BYTES);

          } else if (g.type == AND) {
            curr_head_pos = (gate_offset + curr_and_gate) * thread_params->num_bucket;
            eval_gates_to_blocks.GetExecIDAndIndex(curr_head_pos, curr_head_block, curr_head_idx);
            XOR_128(values + g.out_wire * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx]);

            XOR_128(topological_solderings.get() + (left_gate_start + curr_and_gate) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx]);

            XOR_128(topological_solderings.get() + (right_gate_start + curr_and_gate) * CSEC_BYTES, commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx]);

            XOR_128(topological_solderings.get() + (left_gate_start + curr_and_gate) * CSEC_BYTES, values + g.left_wire * CSEC_BYTES);
            XOR_128(topological_solderings.get() + (right_gate_start + curr_and_gate) * CSEC_BYTES, values + g.right_wire * CSEC_BYTES);

            //Build decommit_info
            std::copy(commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares0[thread_params->out_keys_start + curr_head_idx] + CODEWORD_BYTES, decommit_shares_tmp0 + g.out_wire * CODEWORD_BYTES);
            std::copy(commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->out_keys_start + curr_head_idx] + CODEWORD_BYTES, decommit_shares_tmp1 + g.out_wire * CODEWORD_BYTES);

            std::copy(commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares0[thread_params->left_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares0 + (left_gate_start + curr_and_gate) * CODEWORD_BYTES);
            std::copy(commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->left_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares1 + (left_gate_start + curr_and_gate) * CODEWORD_BYTES);

            std::copy(commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares0[thread_params->right_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares0 + (right_gate_start + curr_and_gate) * CODEWORD_BYTES);
            std::copy(commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx], commit_snds[curr_head_block]->commit_shares1[thread_params->right_keys_start + curr_head_idx] + CODEWORD_BYTES, topsolder_decommit_shares1 + (right_gate_start + curr_and_gate) * CODEWORD_BYTES);

            XOR_CodeWords(topsolder_decommit_shares0 + (left_gate_start + curr_and_gate) * CODEWORD_BYTES, decommit_shares_tmp0 + g.left_wire * CODEWORD_BYTES);
            XOR_CodeWords(topsolder_decommit_shares1 + (left_gate_start + curr_and_gate) * CODEWORD_BYTES, decommit_shares_tmp1 + g.left_wire * CODEWORD_BYTES);

            XOR_CodeWords(topsolder_decommit_shares0 + (right_gate_start + curr_and_gate) * CODEWORD_BYTES, decommit_shares_tmp0 + g.right_wire * CODEWORD_BYTES);
            XOR_CodeWords(topsolder_decommit_shares1 + (right_gate_start + curr_and_gate) * CODEWORD_BYTES, decommit_shares_tmp1 + g.right_wire * CODEWORD_BYTES);

            ++curr_and_gate;
          }
        }

        thread_params->chan.Send(topological_solderings.get(), num_top_solderings * CSEC_BYTES);

        commit_snds[exec_id]->BatchDecommit(topsolder_decommit_shares0, topsolder_decommit_shares1, num_top_solderings);
      }
    });
  }

  for (std::future<void>& r : top_soldering_execs_finished) {
    r.wait();
  }

  auto top_soldering_end = GET_TIME();
#ifdef TINY_PRINT
  PRINT_TIME(top_soldering_end, top_soldering_begin, "TOP_SOLDER");
#endif
}

void TinyConstructor::Online(std::vector<Circuit*>& circuits, std::vector<uint8_t*>& inputs, int eval_num_execs) {

  std::vector<std::future<void>> online_execs_finished(eval_num_execs);
  std::vector<int> circuits_from, circuits_to;

  PartitionBufferFixedNum(circuits_from, circuits_to, eval_num_execs, circuits.size());

  IDMap eval_auths_to_blocks(eval_auths_ids, thread_params_vec[0]->Q + thread_params_vec[0]->A, thread_params_vec[0]->auth_start);
  IDMap eval_gates_to_blocks(eval_gates_ids, thread_params_vec[0]->Q + thread_params_vec[0]->A, thread_params_vec[0]->out_keys_start);

  for (int exec_id = 0; exec_id < eval_num_execs; ++exec_id) {

    int circ_from = circuits_from[exec_id];
    int circ_to = circuits_to[exec_id];
    Params* thread_params = thread_params_vec[exec_id].get();

    online_execs_finished[exec_id] = thread_pool.push([this, thread_params, exec_id, circ_from, circ_to, &circuits, &inputs, &eval_gates_to_blocks, &eval_auths_to_blocks] (int id) {

      Circuit* circuit;
      uint8_t* input;
      uint8_t* eval_inp_keys;
      uint8_t* out_keys;
      uint8_t* decommit_shares_inp_0;
      uint8_t* decommit_shares_inp_1;
      uint8_t* decommit_shares_out_0;
      uint8_t* decommit_shares_out_1;
      uint8_t* e;

      int gate_offset, inp_offset, out_offset;
      int curr_auth_inp_head_pos, curr_inp_head_block, curr_inp_head_idx, curr_output_pos, curr_output_block, curr_output_idx;
      int curr_input, curr_output, ot_commit_block, commit_id;
      for (int c = circ_from; c < circ_to; ++c) {
        circuit = circuits[c];
        input = inputs[c];
        gate_offset = gates_offset[c];
        inp_offset = inputs_offset[c];
        out_offset = outputs_offset[c];

        uint8_t* const_inp_keys = new uint8_t[circuit->num_const_inp_wires * CSEC_BYTES + (circuit->num_eval_inp_wires + circuit->num_out_wires) * (CODEWORD_BYTES + CSEC_BYTES) + BITS_TO_BYTES(circuit->num_eval_inp_wires)];

        decommit_shares_inp_0 = const_inp_keys + circuit->num_const_inp_wires * CSEC_BYTES;
        decommit_shares_inp_1 =  decommit_shares_inp_0 + circuit->num_eval_inp_wires * CODEWORD_BYTES;

        decommit_shares_out_0 =  decommit_shares_inp_1 + circuit->num_eval_inp_wires * CSEC_BYTES;
        decommit_shares_out_1 = decommit_shares_out_0 + circuit->num_out_wires * CODEWORD_BYTES;

        e = decommit_shares_out_1 + circuit->num_out_wires * CSEC_BYTES;

        uint32_t num_send_bytes_inp = circuit->num_const_inp_wires * CSEC_BYTES +  circuit->num_eval_inp_wires * (CODEWORD_BYTES + CSEC_BYTES);
        uint32_t num_send_bytes_out =  circuit->num_out_wires * (CODEWORD_BYTES + CSEC_BYTES);

        //Construct const_inp_keys first
        for (int i = 0; i < circuit->num_const_inp_wires; ++i) {
          curr_auth_inp_head_pos = params.num_pre_gates * thread_params->num_auth + (inp_offset + i) * thread_params->num_inp_auth;
          eval_auths_to_blocks.GetExecIDAndIndex(curr_auth_inp_head_pos, curr_inp_head_block, curr_inp_head_idx);
          XOR_128(const_inp_keys + i * CSEC_BYTES, commit_snds[curr_inp_head_block]->commit_shares0[thread_params->auth_start + curr_inp_head_idx], commit_snds[curr_inp_head_block]->commit_shares1[thread_params->auth_start + curr_inp_head_idx]);
          if (GetBit(i, input)) {
            XOR_128(const_inp_keys + i * CSEC_BYTES, commit_snds[curr_inp_head_block]->commit_shares0[thread_params->delta_pos]);
            XOR_128(const_inp_keys + i * CSEC_BYTES, commit_snds[curr_inp_head_block]->commit_shares1[thread_params->delta_pos]);
          }
        }
        //Do eval_input based on e
        thread_params->chan.ReceiveBlocking(e, BITS_TO_BYTES(circuit->num_eval_inp_wires));

        for (int i = 0; i < circuit->num_eval_inp_wires; ++i) {
          curr_input = (inp_offset + i);
          ot_commit_block = curr_input / thread_params->num_pre_inputs;
          commit_id = thread_params->ot_chosen_start + curr_input % thread_params->num_pre_inputs;

          std::copy(commit_snds[ot_commit_block]->commit_shares0[commit_id], commit_snds[ot_commit_block]->commit_shares0[commit_id] + CODEWORD_BYTES, decommit_shares_inp_0 + i * CODEWORD_BYTES);

          std::copy(commit_snds[ot_commit_block]->commit_shares1[commit_id], commit_snds[ot_commit_block]->commit_shares1[commit_id] + CSEC_BYTES, decommit_shares_inp_1 + i * CSEC_BYTES);

          //Add the input key
          curr_auth_inp_head_pos = params.num_pre_gates * thread_params->num_auth + (inp_offset + circuit->num_const_inp_wires + i) * thread_params->num_inp_auth;
          eval_auths_to_blocks.GetExecIDAndIndex(curr_auth_inp_head_pos, curr_inp_head_block, curr_inp_head_idx);
          XOR_CodeWords(decommit_shares_inp_0 + i * CODEWORD_BYTES, commit_snds[curr_inp_head_block]->commit_shares0[thread_params->auth_start + curr_inp_head_idx]);

          XOR_128(decommit_shares_inp_1 + i * CSEC_BYTES, commit_snds[curr_inp_head_block]->commit_shares1[thread_params->auth_start + curr_inp_head_idx]);

          if (GetBit(i, e)) {
            XOR_CodeWords(decommit_shares_inp_0 + i * CODEWORD_BYTES, commit_snds[ot_commit_block]->commit_shares0[thread_params->delta_pos]);

            XOR_128(decommit_shares_inp_1 + i * CSEC_BYTES, commit_snds[ot_commit_block]->commit_shares1[thread_params->delta_pos]);

          }
        }

        //Send all input keys and decommits
        thread_params->chan.Send(const_inp_keys, num_send_bytes_inp);

        //Construct output key decommits
        for (int i = 0; i < circuit->num_out_wires; ++i) {
          curr_output = (out_offset + i);
          ot_commit_block = curr_output / thread_params->num_pre_outputs;
          commit_id = thread_params->out_lsb_blind_start + curr_output % thread_params->num_pre_outputs;

          std::copy(commit_snds[ot_commit_block]->commit_shares0[commit_id], commit_snds[ot_commit_block]->commit_shares0[commit_id] + CODEWORD_BYTES, decommit_shares_out_0 + i * CODEWORD_BYTES);

          std::copy(commit_snds[ot_commit_block]->commit_shares1[commit_id], commit_snds[ot_commit_block]->commit_shares1[commit_id] + CSEC_BYTES, decommit_shares_out_1 + i * CSEC_BYTES);

          curr_output_pos = (gate_offset + circuit->num_and_gates - circuit->num_out_wires + i) * thread_params->num_bucket;
          eval_gates_to_blocks.GetExecIDAndIndex(curr_output_pos, curr_output_block, curr_output_idx);

          XOR_CodeWords(decommit_shares_out_0 + i * CODEWORD_BYTES, commit_snds[curr_output_block]->commit_shares0[thread_params->out_keys_start + curr_output_idx]);

          XOR_128(decommit_shares_out_1 + i * CSEC_BYTES, commit_snds[curr_output_block]->commit_shares1[thread_params->out_keys_start + curr_output_idx]);
        }

        //Send output decommits
        thread_params->chan.Send(decommit_shares_out_0,  num_send_bytes_out);

        delete[] const_inp_keys; //Deletes everything
      }
    });
  }

  for (std::future<void>& r : online_execs_finished) {
    r.wait();
  }
}

//The below function is essentially a mix of the two CommitSnd member functions ConsistencyCheck and BatchDecommit.
void TinyConstructor::BatchDecommitLSB(CommitSender* commit_snd, uint8_t decommit_shares0[], uint8_t decommit_shares1[], int num_values) {

  //Preprocess DELTA. As we need to transpose columns containing Delta we need the transpose scratch-pads. Notice they are of smaller size than for normal decommit
  int delta_matrix_size = BITS_TO_BYTES(commit_snd->row_dim * commit_snd->col_dim_single); //delta_matrix_size is 8x smaller than transpose_matrix_size.

  std::unique_ptr<uint8_t[]> delta_matrix_tmp0(std::make_unique<uint8_t[]>(4 * delta_matrix_size));
  uint8_t* delta_matrix_tmp1 = delta_matrix_tmp0.get() + 2 * delta_matrix_size;

  //Receive challenge seeds from receiver and load initial challenge delta_chal
  uint8_t ver_leak_challenge[BITS_TO_BYTES(commit_snd->col_dim_single) + CSEC_BYTES];
  commit_snd->params.chan.ReceiveBlocking(ver_leak_challenge, 2 * CSEC_BYTES);
  uint8_t* delta_chal = ver_leak_challenge;
  uint8_t* alpha_seed = ver_leak_challenge + CSEC_BYTES;

  //Load Delta shares into the matrices to be transposed. However only if the bit is set in delta_chal. The remaining columns are left as 0. This reflects adding Delta or not.
  for (int i = 0; i < commit_snd->col_dim_single; ++i) {
    if (GetBit(i, delta_chal)) {
      std::copy(commit_snd->commit_shares0[commit_snd->params.delta_pos], commit_snd->commit_shares0[commit_snd->params.delta_pos] + CODEWORD_BYTES, delta_matrix_tmp0.get() + delta_matrix_size + i * commit_snd->row_dim_bytes);
      std::copy(commit_snd->commit_shares1[commit_snd->params.delta_pos], commit_snd->commit_shares1[commit_snd->params.delta_pos] + CODEWORD_BYTES, delta_matrix_tmp1 + delta_matrix_size + i * commit_snd->row_dim_bytes);
    }
  }
  //Transpose the blocks containing the Delta shares
  transpose_128_320(delta_matrix_tmp0.get() + delta_matrix_size, delta_matrix_tmp0.get(), 1);
  transpose_128_320(delta_matrix_tmp1 + delta_matrix_size, delta_matrix_tmp1, 1);

  //The below proceeds more in line with normal batch decommit.


  //Setup all registers for calculation the linear combinations. Will end up with SSEC_BITS linear combinations.
  uint8_t final_result0[2 * CODEWORD_BYTES * SSEC];
  uint8_t* final_result1 = final_result0 + CODEWORD_BYTES * SSEC;


  //res_tmps is twice as large as we do not do degree reduction until the very end, so we need to accumulate a larger intermediate value. The way we add Delta is to load them into res_totals directly, remember for roughly half of them the all 0 value is added.
  __m128i res_tmp[4][CODEWORD_BITS];
  __m128i res_totals[2][CODEWORD_BITS];
  for (int i = 0; i < CODEWORD_BITS; ++i) {
    res_tmp[0][i] = _mm_setzero_si128();
    res_tmp[1][i] = _mm_setzero_si128();
    res_tmp[2][i] = _mm_setzero_si128();
    res_tmp[3][i] = _mm_setzero_si128();
    res_totals[0][i] = _mm_lddqu_si128((__m128i*) (delta_matrix_tmp0.get() + i * AES_BYTES)); //Apply DELTA shares
    res_totals[1][i] = _mm_lddqu_si128((__m128i*) (delta_matrix_tmp1 + i * AES_BYTES)); //Apply DELTA shares
  }

  __m128i vals[2];
  __m128i vals_result[4];

  //Need four temporary matrices for transposing each block of commitments which are added the the temporary results res_tmp. Each share needs two matrices.
  std::unique_ptr<uint8_t[]> matrices_tmp0(std::make_unique<uint8_t[]>(4 * commit_snd->transpose_matrix_size));
  uint8_t* matrices_tmp1 = matrices_tmp0.get() + 2 * commit_snd->transpose_matrix_size;

  //Load initial challenge alpha
  __m128i alpha = _mm_lddqu_si128((__m128i *) alpha_seed);

  //Compute number of check_blocks needed in total for num_values
  int num_check_blocks = CEIL_DIVIDE(num_values, commit_snd->col_dim);

  //For each check_block we load the decommitments in column-major order and then transpose to get to row-major order so we can address AES_BITS values entry-wise at a time.
  for (int j = 0; j < num_check_blocks; ++j) {
    //Load block
    for (int i = 0; i < commit_snd->col_dim; ++i) {
      int num_check_index = j * commit_snd->col_dim + i;
      if (num_check_index < num_values) {
        std::copy(decommit_shares0 + num_check_index * CODEWORD_BYTES, decommit_shares0 + num_check_index * CODEWORD_BYTES + CODEWORD_BYTES, matrices_tmp0.get() + commit_snd->transpose_matrix_size + i * commit_snd->row_dim_bytes);
        std::copy(decommit_shares1 + num_check_index * CODEWORD_BYTES, decommit_shares1 + num_check_index * CODEWORD_BYTES + CODEWORD_BYTES, matrices_tmp1 + commit_snd->transpose_matrix_size + i * commit_snd->row_dim_bytes);
      }
      else { //this pads the last block with 0 rows
        std::fill(matrices_tmp0.get() + commit_snd->transpose_matrix_size + i * commit_snd->row_dim_bytes, matrices_tmp0.get() + commit_snd->transpose_matrix_size + i * commit_snd->row_dim_bytes + CODEWORD_BYTES, 0);
        std::fill(matrices_tmp1 + commit_snd->transpose_matrix_size + i * commit_snd->row_dim_bytes, matrices_tmp1 + commit_snd->transpose_matrix_size + i * commit_snd->row_dim_bytes + CODEWORD_BYTES, 0);
      }
    }

    //Transpose block
    transpose_128_320(matrices_tmp0.get() + commit_snd->transpose_matrix_size, matrices_tmp0.get(), commit_snd->col_blocks);
    transpose_128_320(matrices_tmp1 + commit_snd->transpose_matrix_size, matrices_tmp1, commit_snd->col_blocks);

    //Compute on block. Processes the block matrices in the same way as for the consistency check with the modification that there is no blinding values
    for (int l = 0; l < commit_snd->col_blocks; ++l) {
      for (int i = 0; i < CODEWORD_BITS; ++i) {
        vals[0] = _mm_lddqu_si128((__m128i*) (matrices_tmp0.get() + l * AES_BYTES + i * commit_snd->col_dim_bytes));
        vals[1] = _mm_lddqu_si128((__m128i*) (matrices_tmp1 + l * AES_BYTES + i * commit_snd->col_dim_bytes));

        if (j * commit_snd->col_dim + l * AES_BITS < num_values - AES_BITS) {
          //The actual commitments are multiplied with alpha
          mul128_karatsuba(vals[0], alpha, &vals_result[0], &vals_result[1]);
          mul128_karatsuba(vals[1], alpha, &vals_result[2], &vals_result[3]);
          //Accumulate the vals_result into res_tmps
          res_tmp[0][i] = _mm_xor_si128(res_tmp[0][i], vals_result[0]);
          res_tmp[1][i] = _mm_xor_si128(res_tmp[1][i], vals_result[1]);
          res_tmp[2][i] = _mm_xor_si128(res_tmp[2][i], vals_result[2]);
          res_tmp[3][i] = _mm_xor_si128(res_tmp[3][i], vals_result[3]);
        } else if (j * commit_snd->col_dim + l * AES_BITS < num_values) {
          //The AES_BITS blinding one-time commitments are added directly to res_totals
          res_totals[0][i] = _mm_xor_si128(res_totals[0][i], vals[0]);
          res_totals[1][i] = _mm_xor_si128(res_totals[1][i], vals[1]);
        }
      }
      //When done with one col_block we square the challenge element alpha. There are 8 col_blocks within each block
      gfmul128_no_refl(alpha, alpha, &alpha);
    }
  }

  //mask is used to select the first SSEC linear combinations from res_totals and store in final_result0 and final_results1. Needed as we actually produce AES_BITS linear combinations due to convenience. However we only send and verify 2*SSEC of these.
  uint8_t mask[CSEC_BYTES] = {0};
  std::fill(mask, mask + SSEC_BYTES, 0xFF);
  __m128i store_mask = _mm_lddqu_si128((__m128i*) mask);

  //Reduce the resulting linear combinations and store in commit_shares_gf
  for (int i = 0; i < CODEWORD_BITS; ++i) {
    gfred128_no_refl(res_tmp[0][i], res_tmp[1][i], &res_tmp[0][i]);
    gfred128_no_refl(res_tmp[2][i], res_tmp[3][i], &res_tmp[2][i]);
    res_totals[0][i] = _mm_xor_si128(res_totals[0][i], res_tmp[0][i]);
    res_totals[1][i] = _mm_xor_si128(res_totals[1][i], res_tmp[2][i]);

    //Finally move the SSEC first linear combinations into final_result0 and final_result1
    _mm_maskmoveu_si128(res_totals[0][i], store_mask, (char*) (final_result0 + i * SSEC_BYTES));
    _mm_maskmoveu_si128(res_totals[1][i], store_mask, (char*) (final_result1 + i * SSEC_BYTES));
  }

  //Send the resulting SSEC decommitments
  commit_snd->params.chan.Send(final_result0, 2 * CODEWORD_BYTES * SSEC);
}