#include "mains/mains.h"
#include "tiny/tiny-constructor.h"

int main(int argc, const char* argv[]) {
  ezOptionParser opt;

  opt.overview = "TinyConstructor Passing Parameters Guide.";
  opt.syntax = "Tinyconst first second third forth fifth sixth";
  opt.example = "Tinyconst -n 4 -c aes -e 8,2,1 -o 0 -ip 10.11.100.216 -p 28001\n\n";
  opt.footer = "ezOptionParser 0.1.4  Copyright (C) 2011 Remik Ziemlinski\nThis program is free and without warranty.\n";

  opt.add(
    "", // Default.
    0, // Required?
    0, // Number of args expected.
    0, // Delimiter if expecting multiple args.
    "Display usage instructions.", // Help description.
    "-h",     // Flag token.
    "-help",  // Flag token.
    "--help", // Flag token.
    "--usage" // Flag token.
  );

  opt.add(
    default_num_iters.c_str(), // Default.
    0, // Required?
    1, // Number of args expected.
    0, // Delimiter if expecting multiple args.
    "Number of circuits to produce and evaluate.", // Help description.
    "-n"
  );

  opt.add(
    default_circuit_name.c_str(), // Default.
    0, // Required?
    1, // Number of args expected.
    0, // Delimiter if expecting multiple args.
    "Circuit name. Can be either aes, sha-1, sha-256 or cbc.", // Help description.
    "-c" // Flag token.
  );

  opt.add(
    default_execs.c_str(), // Default.
    0, // Required?
    3, // Number of args expected.
    ',', // Delimiter if expecting multiple args.
    "Number of parallel executions for each phase. Preprocessing, Offline and Online.", // Help description.
    "-e"
  );

  opt.add(
    default_optimize_online.c_str(), // Default.
    0, // Required?
    1, // Number of args expected.
    0, // Delimiter if expecting multiple args.
    "Optimize for online or overall efficiency", // Help description.
    "-o"
  );

  opt.add(
    default_ip_address.c_str(), // Default.
    0, // Required?
    1, // Number of args expected.
    0, // Delimiter if expecting multiple args.
    "IP Address of Machine running TinyConst", // Help description.
    "-ip"
  );

  opt.add(
    default_port.c_str(), // Default.
    0, // Required?
    1, // Number of args expected.
    0, // Delimiter if expecting multiple args.
    "Port to listen on/connect to", // Help description.
    "-p"
  );

  //Attempt to parse input
  opt.parse(argc, argv);

  //Check if help was requested and do some basic validation
  if (opt.isSet("-h")) {
    Usage(opt);
    return 1;
  }
  std::vector<std::string> badOptions;
  if (!opt.gotExpected(badOptions)) {
    for (int i = 0; i < badOptions.size(); ++i)
      std::cerr << "ERROR: Got unexpected number of arguments for option " << badOptions[i] << ".\n\n";
    Usage(opt);
    return 1;
  }

  //Copy inputs into the right variables
  int num_iters, pre_num_execs, offline_num_execs, online_num_execs, optimize_online, port;
  std::vector<int> num_execs;
  std::string circuit_name, ip_address, exec_name;
  Circuit circuit;
  FILE* fileptr;
  uint8_t* input_buffer;
  long filelen;

  opt.get("-n")->getInt(num_iters);
  opt.get("-c")->getString(circuit_name);
  
  opt.get("-e")->getInts(num_execs);
  pre_num_execs = num_execs[0];
  offline_num_execs = num_execs[1];
  online_num_execs = num_execs[2];

  opt.get("-o")->getInt(optimize_online);
  opt.get("-ip")->getString(ip_address);
  opt.get("-p")->getInt(port);

  //Set the circuit variables according to circuit_name
  if (circuit_name.find("aes") != std::string::npos) {
    exec_name = "AES";
    circuit = read_text_circuit("test/data/AES-non-expanded.txt");
    fileptr = fopen("test/data/aes_input_0.bin", "rb");
  } else if (circuit_name.find("sha-256") != std::string::npos) {
    exec_name = "SHA-256";
    circuit = read_text_circuit("test/data/sha-256.txt");
    fileptr = fopen("test/data/sha256_input_0.bin", "rb");
  } else if (circuit_name.find("sha-1") != std::string::npos) {
    exec_name = "SHA-1";
    circuit = read_text_circuit("test/data/sha-1.txt");
    fileptr = fopen("test/data/sha1_input_0.bin", "rb");
  } else if (circuit_name.find("cbc") != std::string::npos) {
    exec_name = "AES-CBC-MAC";
    circuit = read_text_circuit("test/data/aescbcmac16.txt");
    fileptr = fopen("test/data/cbc_input_0.bin", "rb");
  } else {
    std::cout << "No circuit matching: " << exec_name << ". Terminating" << std::endl;
    return 1;
  }

  //Read input from file to input_buffer and then set it to const_input
  fseek(fileptr, 0, SEEK_END);
  filelen = ftell(fileptr);
  rewind(fileptr);
  input_buffer = new uint8_t[(filelen + 1)];
  fread(input_buffer, filelen, 1, fileptr);

  std::unique_ptr<uint8_t[]> const_input(std::make_unique<uint8_t[]>(BITS_TO_BYTES(circuit.num_const_inp_wires)));
  //Read input the "right" way
  for (int i = 0; i < circuit.num_const_inp_wires; ++i) {
    if (GetBitReversed(i, input_buffer)) {
      SetBit(i, 1, const_input.get());
    } else {
      SetBit(i, 0, const_input.get());
    }
  }

  //Compute number of gates, inputs and outputs that are to be preprocessed
  uint64_t num_gates = num_iters * circuit.num_and_gates;
  uint64_t num_inputs = num_iters * circuit.num_inp_wires;
  uint64_t num_outputs = num_iters * circuit.num_out_wires;

  std::vector<Circuit*> circuits;
  std::vector<uint8_t*> const_inputs;
  for (int i = 0; i < num_iters; ++i) {
    circuits.emplace_back(&circuit);
    const_inputs.emplace_back(const_input.get());
  }

  //Compute the required number of params that are to be created. We create one main param and one for each sub-thread that will be spawned later on. Need to know this at this point to setup context properly
  int num_params = std::max(pre_num_execs, offline_num_execs);
  num_params = std::max(num_params, online_num_execs);
  zmq::context_t context(NUM_IO_THREADS, 2 * (num_params + 1)); //We need two sockets pr. channel

  //Setup the main params object
  Params params(constant_seeds[0], num_gates, num_inputs, num_outputs, ip_address, (uint16_t) port, 0, context, pre_num_execs, GLOBAL_PARAMS_CHAN, optimize_online);

  TinyConstructor tiny_const(params);

  //Warm up network!
  uint8_t* dummy_val = new uint8_t[network_dummy_size]; //50 MB
  uint8_t* dummy_val_rec = new uint8_t[network_dummy_size]; //50 MB
  channel chan(OT_ADMIN_CHANNEL - 1, tiny_const.ot_snd.net.rcvthread, tiny_const.ot_snd.net.sndthread);
  chan.send(dummy_val, network_dummy_size);
  uint8_t* dummy_val_rec2 = chan.blocking_receive();
  params.chan.Send(dummy_val, network_dummy_size);
  params.chan.ReceiveBlocking(dummy_val_rec2, network_dummy_size);
  params.chan.bytes_received_vec[params.chan.received_pointer] = 0;
  params.chan.bytes_sent_vec[params.chan.sent_pointer] = 0;
  delete[] dummy_val;
  delete[] dummy_val_rec;
  free(dummy_val_rec2);
  //Warm up network!

  //Run initial Setup (BaseOT) phase
  auto setup_begin = GET_TIME();
  mr_init_threading(); //Needed for Miracl library to work with threading.
  tiny_const.Setup();
  mr_end_threading();
  auto setup_end = GET_TIME();
  

  //Run Preprocessing phase
  auto preprocess_begin = GET_TIME();
  tiny_const.Preprocess();
  auto preprocess_end = GET_TIME();

  //Preprocessing creates pre_num_execs sub-param objects. If more are needed in the offline and online phases we create them here.
  int extra_execs = num_params - params.num_execs;
  if (extra_execs < 0) {
  } else {
    std::unique_ptr<uint8_t[]> extra_thread_seeds(std::make_unique<uint8_t[]>(extra_execs * CSEC_BYTES));
    tiny_const.params.rnd.GenRnd(extra_thread_seeds.get(), extra_execs * CSEC_BYTES);
    for (int i = 0; i < extra_execs; ++i) {
      tiny_const.thread_params_vec.emplace_back(std::make_unique<Params>(tiny_const.params, extra_thread_seeds.get() + i * CSEC_BYTES, tiny_const.thread_params_vec[0]->num_pre_gates, tiny_const.thread_params_vec[0]->num_pre_inputs, tiny_const.thread_params_vec[0]->num_pre_outputs, params.num_execs + i));
      tiny_const.commit_snds.emplace_back(std::make_unique<CommitSender>(*tiny_const.thread_params_vec[params.num_execs + i], tiny_const.rot_seeds0.get(), tiny_const.rot_seeds1));
    }
  }

  // Figure out how many executions to run in offline phase
  int top_num_execs = std::min((int)circuits.size(), offline_num_execs);
  if (top_num_execs == 1) {
    tiny_const.thread_pool.resize(top_num_execs);
  }

  // tiny_const.thread_pool.resize(params.num_cpus * TP_MUL_FACTOR); //Very high performance benefit if on a high latency network as more executions can run in parallel!

  //Run Offline phase
  auto offline_begin = GET_TIME();
  tiny_const.Offline(circuits, top_num_execs);
  auto offline_end = GET_TIME();

  // If we are doing single evaluation then we have slightly better performance with a single thread running in the thread pool.
  int eval_num_execs = std::min((int)circuits.size(), online_num_execs);
  if (eval_num_execs == 1) {
    tiny_const.thread_pool.resize(eval_num_execs);
  }

  //Run Online phase
  auto online_begin = GET_TIME();
  tiny_const.Online(circuits, const_inputs, eval_num_execs);
  auto online_end = GET_TIME();

  // Average out the timings of each phase and print results
  uint64_t setup_time_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(setup_end - setup_begin).count();
  uint64_t preprocess_time_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(preprocess_end - preprocess_begin).count();
  uint64_t offline_time_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(offline_end - offline_begin).count();
  uint64_t online_time_nano = std::chrono::duration_cast<std::chrono::nanoseconds>(online_end - online_begin).count();

  std::cout << "===== Const timings for " << num_iters << " x " << exec_name << "(" << (num_iters * circuit.num_and_gates) << ") with " << pre_num_execs << " preprocessing execs, " << top_num_execs << " offline execs and " << eval_num_execs << " online execs =====" << std::endl;

  std::cout << "Setup ms: " << (double) setup_time_nano / num_iters / 1000000 << std::endl;
  std::cout << "Preprocess ms: " << (double) preprocess_time_nano / num_iters / 1000000 << std::endl;
  std::cout << "Offline ms: " << (double) offline_time_nano / num_iters / 1000000 << std::endl;
  std::cout << "Online ms: " << (double) online_time_nano / num_iters / 1000000 << std::endl;
  
  return 0;
}