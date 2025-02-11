#include "mpt.h"

#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include <stdarg.h>

std::stringstream oss;
std::string log_message;

void cleasr_log_stream() {
  oss.flush();
  // Clearing the contents
  oss.str(""); // Setting its content to an empty string

  // Resetting the error flags
  oss.clear();
  log_message = "";
}

// load the model's weights from a file
bool mpt_model_load(Mpt &mpt_ctx, const std::string &fname, mpt_model &model,
                    gpt_vocab &vocab) {
  oss << __func__ << ": loading model from '" << fname
      << "' - please wait ...\n";
  log_message = oss.str();

  mpt_ctx.OnLogMessage(log_message);
  cleasr_log_stream();

  auto fin = std::ifstream(fname, std::ios::binary);
  if (!fin) {
    oss << "error " << __func__ << ": failed to open '" << fname << "'\n";
    log_message = oss.str();

    mpt_ctx.OnLogMessage(log_message);
    cleasr_log_stream();

    return false;
  }

  // verify magic
  {
    uint32_t magic;
    fin.read((char *)&magic, sizeof(magic));
    if (magic != GGML_FILE_MAGIC) {
      oss << "error " << __func__ << ": invalid model file '" << fname
          << "' (bad magic)\n";
      log_message = oss.str();

      mpt_ctx.OnLogMessage(log_message);
      cleasr_log_stream();

      return false;
    }
  }

  // load hparams
  {
    auto &hparams = model.hparams;

    fin.read((char *)&hparams.d_model, sizeof(hparams.d_model));
    fin.read((char *)&hparams.max_seq_len, sizeof(hparams.max_seq_len));
    fin.read((char *)&hparams.n_heads, sizeof(hparams.n_heads));
    fin.read((char *)&hparams.n_layers, sizeof(hparams.n_layers));
    fin.read((char *)&hparams.n_vocab, sizeof(hparams.n_vocab));
    fin.read((char *)&hparams.alibi_bias_max, sizeof(hparams.alibi_bias_max));
    fin.read((char *)&hparams.clip_qkv, sizeof(hparams.clip_qkv));
    fin.read((char *)&hparams.ftype, sizeof(hparams.ftype));

    hparams.n_ctx = std::min(hparams.max_seq_len, hparams.n_ctx);

    const int32_t qntvr = hparams.ftype / GGML_QNT_VERSION_FACTOR;

    oss << __func__ << ": d_model        = " << hparams.d_model << "\n"
        << __func__ << ": max_seq_len    = " << hparams.max_seq_len << "\n"
        << __func__ << ": n_ctx          = " << hparams.n_ctx << "\n"
        << __func__ << ": n_heads        = " << hparams.n_heads << "\n"
        << __func__ << ": n_layers       = " << hparams.n_layers << "\n"
        << __func__ << ": n_vocab        = " << hparams.n_vocab << "\n"
        << __func__ << ": alibi_bias_max = " << hparams.alibi_bias_max << "\n"
        << __func__ << ": clip_qkv       = " << hparams.clip_qkv << "\n"
        << __func__ << ": ftype          = " << hparams.ftype << "\n"
        << __func__ << ": qntvr          = " << qntvr << "\n";

    log_message = oss.str();

    mpt_ctx.OnLogMessage(log_message);
    cleasr_log_stream();
    hparams.ftype %= GGML_QNT_VERSION_FACTOR;
  }

  // load vocab
  {
    const int32_t n_vocab = model.hparams.n_vocab;

    std::string word;
    std::vector<char> buf(128);

    for (int i = 0; i < n_vocab; i++) {
      uint32_t len;
      fin.read((char *)&len, sizeof(len));

      buf.resize(len);
      fin.read((char *)buf.data(), len);
      word.assign(buf.data(), len);

      // Convert token from utf-8
      std::wstring word_multibytes = convert_to_wstring(word);
      word.resize(word_multibytes.size());
      for (int w = 0; w < word_multibytes.size(); w++) {
        word[w] = uint8_t(word_multibytes[w]);
      }

      vocab.token_to_id[word] = i;
      vocab.id_to_token[i] = word;
    }
  }

  // for the big tensors, we have the option to store the data in 16-bit
  // floats or quantized in order to save memory and also to speed up the
  // computation
  ggml_type wtype = ggml_ftype_to_ggml_type((ggml_ftype)(model.hparams.ftype));
  if (wtype == GGML_TYPE_COUNT) {
    oss << "error " << __func__ << ": invalid model file '" << fname
        << "' (bad ftype value " << model.hparams.ftype << ")\n";

    log_message = oss.str();
    
    mpt_ctx.OnLogMessage(log_message);
	cleasr_log_stream();
    return false;
  }

  auto &ctx = model.ctx;

  size_t ctx_size = 0;

  const auto &hparams = model.hparams;
  const size_t n_ctx = hparams.n_ctx;

  {
    const size_t n_embd = hparams.d_model;
    const size_t n_layer = hparams.n_layers;
    const size_t n_vocab = hparams.n_vocab;

    ctx_size += n_embd * n_vocab * ggml_type_sizef(wtype); // wte_weight
    ctx_size += n_embd * ggml_type_sizef(GGML_TYPE_F32);   // norm_f_weight

    ctx_size +=
        n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // ln_1_weight
    ctx_size += n_layer * (3 * n_embd * n_embd *
                           ggml_type_sizef(wtype)); // attn_Wqkv_weight
    ctx_size += n_layer * (n_embd * n_embd *
                           ggml_type_sizef(wtype)); // attn_out_proj_weight
    ctx_size +=
        n_layer * (n_embd * ggml_type_sizef(GGML_TYPE_F32)); // ln_2_weight
    ctx_size += n_layer * (4 * n_embd * n_embd *
                           ggml_type_sizef(wtype)); // mlp_mlp_up_weight
    ctx_size += n_layer * (n_embd * n_embd * 4 *
                           ggml_type_sizef(wtype)); // mlp_mlp_down_weight

    ctx_size +=
        n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F16); // memory_k
    ctx_size +=
        n_ctx * n_layer * n_embd * ggml_type_sizef(GGML_TYPE_F16); // memory_v

    ctx_size += (1 + 6 * n_layer) * 512; // object overhead

    oss << __func__ << ": ggml ctx size = " << std::fixed
        << std::setprecision(2) << ctx_size / (1024.0 * 1024.0) << " MB\n";

    log_message = oss.str();

    mpt_ctx.OnLogMessage(log_message);
    cleasr_log_stream();
  }

  // create the ggml context
  {
    struct ggml_init_params params = {
        /*.mem_size   =*/ctx_size,
        /*.mem_buffer =*/NULL,
        /*.no_alloc   =*/false,
    };

    model.ctx = ggml_init(params);
    if (!model.ctx) {
      oss << "error " << __func__ << ": ggml_init() failed\n";

      log_message = oss.str();

      mpt_ctx.OnLogMessage(log_message);
      cleasr_log_stream();
      return false;
    }
  }

  // prepare memory for the weights
  {
    const auto &hparams = model.hparams;

    const size_t n_embd = hparams.d_model;
    const size_t n_layer = hparams.n_layers;
    const size_t n_vocab = hparams.n_vocab;

    model.layers.resize(n_layer);

    model.wte_weight = ggml_new_tensor_2d(ctx, wtype, n_embd, n_vocab);
    model.norm_f_weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);

    // map by name
    model.tensors["transformer.wte.weight"] = model.wte_weight;
    model.tensors["transformer.norm_f.weight"] = model.norm_f_weight;

    for (int i = 0; i < (int)n_layer; ++i) {
      auto &layer = model.layers[i];

      layer.norm_1_weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
      layer.c_attn_wqkv_weight =
          ggml_new_tensor_2d(ctx, wtype, n_embd, 3 * n_embd);
      layer.c_attn_out_proj_weight =
          ggml_new_tensor_2d(ctx, wtype, n_embd, n_embd);
      layer.norm_2_weight = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, n_embd);
      layer.ffn_up_proj = ggml_new_tensor_2d(ctx, wtype, n_embd, 4 * n_embd);
      layer.ffn_down_proj = ggml_new_tensor_2d(ctx, wtype, 4 * n_embd, n_embd);

      // map by name
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".norm_1.weight"] = layer.norm_1_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".attn.Wqkv.weight"] = layer.c_attn_wqkv_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".attn.out_proj.weight"] = layer.c_attn_out_proj_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".norm_2.weight"] = layer.norm_2_weight;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".ffn.up_proj.weight"] = layer.ffn_up_proj;
      model.tensors["transformer.blocks." + std::to_string(i) +
                    ".ffn.down_proj.weight"] = layer.ffn_down_proj;
    }
  }

  // key + value memory
  {
    const auto &hparams = model.hparams;

    const size_t n_embd = hparams.d_model;
    const size_t n_layer = hparams.n_layers;

    const int64_t n_mem = n_layer * n_ctx;
    const int64_t n_elements = n_embd * n_mem;

    model.memory_k = ggml_new_tensor_1d(ctx, GGML_TYPE_F16, n_elements);
    model.memory_v = ggml_new_tensor_1d(ctx, GGML_TYPE_F16, n_elements);

    const size_t memory_size =
        ggml_nbytes(model.memory_k) + ggml_nbytes(model.memory_v);

    oss << __func__ << ": memory_size = " << std::fixed << std::setprecision(2)
        << memory_size / 1024.0 / 1024.0 << " MB, n_mem = " << n_mem << "\n";

    log_message = oss.str();

    mpt_ctx.OnLogMessage(log_message);
    cleasr_log_stream();
  }

  // load weights
  {
    int n_tensors = 0;
    size_t total_size = 0;

    oss << __func__ << ": ";

    log_message = oss.str();

    mpt_ctx.OnLogMessage(log_message);
    cleasr_log_stream();

    while (true) {
      int32_t n_dims;
      int32_t length;
      int32_t ttype;

      fin.read(reinterpret_cast<char *>(&n_dims), sizeof(n_dims));
      fin.read(reinterpret_cast<char *>(&length), sizeof(length));
      fin.read(reinterpret_cast<char *>(&ttype), sizeof(ttype));

      if (fin.eof()) {
        break;
      }

      int32_t nelements = 1;
      int32_t ne[2] = {1, 1};
      for (int i = 0; i < n_dims; ++i) {
        fin.read(reinterpret_cast<char *>(&ne[i]), sizeof(ne[i]));
        nelements *= ne[i];
      }

      std::string name(length, 0);
      fin.read(&name[0], length);

      if (model.tensors.find(name) == model.tensors.end()) {
        oss << "error " << __func__ << ": unknown tensor '" << name
            << "' in model file\n";

        log_message = oss.str();
        mpt_ctx.OnLogMessage(log_message);
        cleasr_log_stream();
        return false;
      }

      auto tensor = model.tensors[name];
      if (ggml_nelements(tensor) != nelements) {

        oss << "error " << __func__ << ": tensor '" << name
            << "' has wrong size in model file\n";

        log_message = oss.str();

        mpt_ctx.OnLogMessage(log_message);
        cleasr_log_stream();
        return false;
      }

      if (tensor->ne[0] != ne[0] || tensor->ne[1] != ne[1]) {
        oss << "error " << __func__ << ": tensor '" << name
            << "' has wrong shape in model file: got [" << std::setw(5)
            << (int)tensor->ne[0] << ", " << std::setw(5) << (int)tensor->ne[1]
            << "], expected [" << std::setw(5) << ne[0] << ", " << std::setw(5)
            << ne[1] << "]\n";

        log_message = oss.str();

        mpt_ctx.OnLogMessage(log_message);
        cleasr_log_stream();
        return false;
      }

      // for debugging
      if (0) {
        oss << std::setw(24) << name << " - [" << std::setw(5) << ne[0] << ", "
            << std::setw(5) << ne[1] << "], type = " << std::setw(6)
            << ggml_type_name(ggml_type(ttype)) << ", " << std::fixed
            << std::setprecision(2) << ggml_nbytes(tensor) / 1024.0 / 1024.0
            << " MB"
            << ", " << std::setw(9) << ggml_nbytes(tensor) << " bytes";

        log_message = oss.str();
        cleasr_log_stream();
        mpt_ctx.OnLogMessage(log_message);
        cleasr_log_stream();
      }

      const size_t bpe = ggml_type_size(ggml_type(ttype));

      if ((nelements * bpe) / ggml_blck_size(tensor->type) !=
          ggml_nbytes(tensor)) {
        oss << "error " << __func__ << ": tensor '" << name
            << "' has wrong size in model file: got " << ggml_nbytes(tensor)
            << ", expected " << nelements * bpe << "\n";

        log_message = oss.str();
        mpt_ctx.OnLogMessage(log_message);
        cleasr_log_stream();
        return false;
      }

      fin.read(reinterpret_cast<char *>(tensor->data), ggml_nbytes(tensor));

      total_size += ggml_nbytes(tensor);
      if (++n_tensors % 8 == 0) {
        mpt_ctx.OnLogMessage(".");
      }
    }

    oss << " done\n"
        << __func__ << ": model size = " << std::fixed << std::setprecision(2)
        << total_size / 1024.0 / 1024.0 << " MB / num tensors = " << n_tensors
        << "\n";
    log_message = oss.str();
    mpt_ctx.OnLogMessage(log_message);
    cleasr_log_stream();
  }

  fin.close();

  return true;
}

// evaluate the transformer
//
//   - model:     the model
//   - n_threads: number of threads to use
//   - n_past:    the context size so far
//   - embd_inp:  the embeddings of the tokens in the context
//   - embd_w:    the predicted logits for the next token
//
bool mpt_eval(Mpt &mpt_ctx, const mpt_model &model, const int n_threads,
              const int n_past, const std::vector<gpt_vocab::id> &embd_inp,
              std::vector<float> &embd_w, bool logits_all,
              size_t &mem_per_token) {
  const int N = embd_inp.size();

  const auto &hparams = model.hparams;

  const int n_embd = hparams.d_model;
  const int n_layer = hparams.n_layers;
  const int n_head = hparams.n_heads;
  const int n_vocab = hparams.n_vocab;
  const int n_ctx = hparams.n_ctx;

  static size_t buf_size = 256u * 1024 * 1024;
  static void *buf = malloc(buf_size);

  // use 2 scratch buffers
  // TODO: very hacky solution - reimplement in a more elegant way
  static size_t scr0_size = 256u * 1024 * 1024;
  static void *scr0 = malloc(scr0_size);

  static size_t scr1_size = 256u * 1024 * 1024;
  static void *scr1 = malloc(scr1_size);

  if (mem_per_token > 0 && mem_per_token * N > buf_size) {
    const size_t buf_size_new =
        1.1 *
        (mem_per_token * N); // add 10% to account for ggml object overhead
    // printf("\n%s: reallocating buffer from %zu to %zu bytes\n", __func__,
    // buf_size, buf_size_new);

    // reallocate
    buf_size = buf_size_new;
    buf = realloc(buf, buf_size);
    if (buf == nullptr) {
      oss << "error " << __func__ << ": failed to allocate " << buf_size
          << " bytes\n";

      log_message = oss.str();
      mpt_ctx.OnLogMessage(log_message);
      cleasr_log_stream();
      return false;
    }
  }

  struct ggml_init_params params = {
      /*.mem_size   =*/buf_size,
      /*.mem_buffer =*/buf,
      /*.no_alloc   =*/false,
  };

  struct ggml_context *ctx0 = ggml_init(params);
  struct ggml_cgraph gf = {};

  struct ggml_tensor *embd = ggml_new_tensor_1d(ctx0, GGML_TYPE_I32, N);
  memcpy(embd->data, embd_inp.data(), N * ggml_element_size(embd));

  struct ggml_tensor *inpL = ggml_get_rows(ctx0, model.wte_weight, embd);

  for (int il = 0; il < n_layer; ++il) {

    struct ggml_tensor *cur;

    ggml_set_scratch(ctx0, {
                               0,
                               scr0_size,
                               scr0,
                           });

    // a = self.ln_1(x)
    {
      cur = ggml_norm(ctx0, inpL);

      cur = ggml_mul(
          ctx0, ggml_repeat(ctx0, model.layers[il].norm_1_weight, cur), cur);
    }

    // self-attention
    //  b, _, past_key_value = self.attn(a, past_key_value=past_key_value,
    //  attn_bias=attn_bias, attention_mask=attention_mask,
    //  is_causal=is_causal)
    {
      // compute QKV
      cur = ggml_mul_mat(ctx0, model.layers[il].c_attn_wqkv_weight, cur);

      if (model.hparams.clip_qkv > 0.0f) {
        cur = ggml_clamp(ctx0, cur, -model.hparams.clip_qkv,
                         model.hparams.clip_qkv);
      }

      struct ggml_tensor *Qcur = ggml_view_2d(ctx0, cur, n_embd, N, cur->nb[1],
                                              0 * sizeof(float) * n_embd);
      struct ggml_tensor *Kcur = ggml_view_2d(ctx0, cur, n_embd, N, cur->nb[1],
                                              1 * sizeof(float) * n_embd);
      struct ggml_tensor *Vcur = ggml_view_2d(ctx0, cur, n_embd, N, cur->nb[1],
                                              2 * sizeof(float) * n_embd);

      // store key and value to memory
      {
        struct ggml_tensor *k =
            ggml_view_1d(ctx0, model.memory_k, N * n_embd,
                         (ggml_element_size(model.memory_k) * n_embd) *
                             (il * n_ctx + n_past));
        struct ggml_tensor *v =
            ggml_view_1d(ctx0, model.memory_v, N * n_embd,
                         (ggml_element_size(model.memory_v) * n_embd) *
                             (il * n_ctx + n_past));

        ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Kcur, k));
        ggml_build_forward_expand(&gf, ggml_cpy(ctx0, Vcur, v));
      }

      // Q = Qcur.contiguous().view(n_embd/n_head, n_head, N).permute(0,
      // 2, 1, 3) [64, N, 12]
      struct ggml_tensor *Q =
          ggml_permute(ctx0,
                       ggml_cpy(ctx0, Qcur,
                                ggml_new_tensor_3d(ctx0, GGML_TYPE_F32,
                                                   n_embd / n_head, n_head, N)),
                       0, 2, 1, 3);

      // K = Kmem.view(n_embd/n_head, n_head, n_past + N).permute(0, 2, 1,
      // 3) [64, n_past + N, 12]
      struct ggml_tensor *K = ggml_permute(
          ctx0,
          ggml_reshape_3d(
              ctx0,
              ggml_view_1d(ctx0, model.memory_k, (n_past + N) * n_embd,
                           il * n_ctx * ggml_element_size(model.memory_k) *
                               n_embd),
              n_embd / n_head, n_head, n_past + N),
          0, 2, 1, 3);
      // K * Q
      struct ggml_tensor *KQ = ggml_mul_mat(ctx0, K, Q);

      // KQ_scaled = KQ / sqrt(n_embd/n_head)
      struct ggml_tensor *KQ_scaled = ggml_scale(
          ctx0, KQ, ggml_new_f32(ctx0, 1.0f / sqrt(float(n_embd) / n_head)));

      struct ggml_tensor *KQ_scaled_alibi = ggml_alibi(
          ctx0, KQ_scaled, n_past, n_head, model.hparams.alibi_bias_max);

      // KQ_masked = mask_past(KQ_scaled)
      struct ggml_tensor *KQ_masked =
          ggml_diag_mask_inf(ctx0, KQ_scaled_alibi, n_past);

      // KQ = soft_max(KQ_masked)
      struct ggml_tensor *KQ_soft_max = ggml_soft_max(ctx0, KQ_masked);

      // V_trans = Vmem.view(n_embd/n_head, n_head, n_past + N).permute(1,
      // 2, 0, 3).contiguous() [n_past + N, 64, 12]
      struct ggml_tensor *V_trans = ggml_cpy(
          ctx0,
          ggml_permute(
              ctx0,
              ggml_reshape_3d(
                  ctx0,
                  ggml_view_1d(ctx0, model.memory_v, (n_past + N) * n_embd,
                               il * n_ctx * ggml_element_size(model.memory_v) *
                                   n_embd),
                  n_embd / n_head, n_head, n_past + N),
              1, 2, 0, 3),
          ggml_new_tensor_3d(ctx0, model.memory_v->type, n_past + N,
                             n_embd / n_head, n_head));

      // KQV = transpose(V) * KQ_soft_max
      struct ggml_tensor *KQV = ggml_mul_mat(ctx0, V_trans, KQ_soft_max);

      // KQV_merged = KQV.permute(0, 2, 1, 3)
      struct ggml_tensor *KQV_merged = ggml_permute(ctx0, KQV, 0, 2, 1, 3);

      // cur = KQV_merged.contiguous().view(n_embd, N)
      cur = ggml_cpy(ctx0, KQV_merged,
                     ggml_new_tensor_2d(ctx0, GGML_TYPE_F32, n_embd, N));

      // projection
      {
        cur = ggml_mul_mat(ctx0, model.layers[il].c_attn_out_proj_weight, cur);
      }
    }

    inpL = ggml_add(ctx0, inpL, cur);

    ggml_set_scratch(ctx0, {
                               0,
                               scr1_size,
                               scr1,
                           });

    // m = self.ln_2(x)
    {
      cur = ggml_norm(ctx0, inpL);

      cur = ggml_mul(
          ctx0, ggml_repeat(ctx0, model.layers[il].norm_2_weight, cur), cur);
    }

    // n = self.mlp(m)
    {

      cur = ggml_mul_mat(ctx0, model.layers[il].ffn_up_proj, cur);

      // GELU activation
      cur = ggml_gelu(ctx0, cur);

      // projection
      // cur = proj_w*cur + proj_b
      cur = ggml_mul_mat(ctx0, model.layers[il].ffn_down_proj, cur);
    }

    // x = x + n
    inpL = ggml_add(ctx0, inpL, cur);
  }

  ggml_set_scratch(ctx0, {
                             0,
                             scr0_size,
                             scr0,
                         });

  // norm
  {
    inpL = ggml_norm(ctx0, inpL);
    // inpL = ln_f_g*inpL
    inpL = ggml_mul(ctx0, ggml_repeat(ctx0, model.norm_f_weight, inpL), inpL);
  }

  ggml_set_scratch(ctx0, {
                             0,
                             0,
                             nullptr,
                         });

  // output embedding weight tied to input embedding
  inpL = ggml_mul_mat(ctx0, model.wte_weight, inpL);

  // logits -> probs
  // inpL = ggml_soft_max(ctx0, inpL);

  // run the computation
  ggml_build_forward_expand(&gf, inpL);
  ggml_graph_compute_with_ctx(ctx0, &gf, n_threads);

  // std::cout << "Qcur" << std::endl;
  // print_tensor(Qcur);

  // if (n_past%100 == 0) {
  // ggml_graph_print(&gf);
  // ggml_graph_dump_dot(&gf, NULL, "mpt-model.dot");
  // }

  if (logits_all) {
    // return result for all tokens
    embd_w.resize(n_vocab * N);
    memcpy(embd_w.data(), (float *)ggml_get_data(inpL),
           sizeof(float) * n_vocab * N);
  } else {
    // return result for just the last token
    embd_w.resize(n_vocab);
    memcpy(embd_w.data(), (float *)ggml_get_data(inpL) + (n_vocab * (N - 1)),
           sizeof(float) * n_vocab);
  }

  if (mem_per_token == 0) {
    mem_per_token = ggml_used_mem(ctx0) / N;
  }
  // printf("used_mem = %zu\n", ggml_used_mem(ctx0));

  ggml_free(ctx0);

  return true;
}

std::vector<float> softmax(const std::vector<float> &logits) {
  std::vector<float> probs(logits.size());
  float max_logit = logits[0];
  for (float v : logits)
    max_logit = std::max(max_logit, v);
  double sum_exp = 0.0;
  for (size_t i = 0; i < logits.size(); i++) {
    // Subtract the maximum logit value from the current logit value for
    // numerical stability
    const float logit = logits[i] - max_logit;
    const float exp_logit = expf(logit);
    sum_exp += exp_logit;
    probs[i] = exp_logit;
  }
  for (size_t i = 0; i < probs.size(); i++)
    probs[i] /= sum_exp;
  return probs;
}

void Mpt::LogPerplexity(const std::string &message) {

  int64_t t_predict_us = 0;

  std::vector<float> logits;

  // tokenize the prompt
  std::vector<int> embd_inp = ::gpt_tokenize(vocab, message);

  oss << __func__ << ": number of tokens in prompt = " << embd_inp.size()
      << "\n";

  log_message = oss.str();
  OnLogMessage(log_message);
  cleasr_log_stream();
  // determine the required inference memory per token:
  size_t mem_per_token = 0;
  mpt_eval(*this, model, params.n_threads, 0, {0, 1, 2, 3}, logits, false,
           mem_per_token);

  int count = 0;

  const int n_chunk = embd_inp.size() / params.n_ctx;

  const int n_vocab = model.hparams.n_vocab;
  const int n_batch = params.n_batch;

  double nll = 0.0;
  oss << "error " << __func__ << ": calculating perplexity over " << n_chunk
      << " chunks, batch_size=" << n_batch << "\n";

  log_message = oss.str();

  OnLogMessage(log_message);
  cleasr_log_stream();
  for (int i = 0; i < n_chunk; ++i) {

    const int start = i * params.n_ctx;
    const int end = start + params.n_ctx;

    const int num_batches = (params.n_ctx + n_batch - 1) / n_batch;

    std::vector<float> logits;

    const auto t_start = std::chrono::high_resolution_clock::now();

    for (int j = 0; j < num_batches; ++j) {

      const int batch_start = start + j * n_batch;
      const int batch_size = std::min(end - batch_start, n_batch);

      std::vector<gpt_vocab::id> embd;

      for (int p = 0; p < batch_size; p++) {
        embd.push_back(embd_inp[batch_start + p]);
      }

      std::vector<float> batch_logits; // = llama_get_logits(ctx);

      const int64_t t_start_us = ggml_time_us();

      if (!mpt_eval(*this, model, params.n_threads, j * batch_size, embd,
                    batch_logits, true, mem_per_token)) {
        oss << "error " << __func__ << ": failed to evaluate model\n";

        log_message = oss.str();
        OnLogMessage(log_message);
        cleasr_log_stream();
        return;
      }

      t_predict_us += ggml_time_us() - t_start_us;

      logits.insert(logits.end(), batch_logits.data(),
                    batch_logits.data() + batch_size * n_vocab);
    }

    const auto t_end = std::chrono::high_resolution_clock::now();

    if (i == 0) {
      const float t_total =
          std::chrono::duration<float>(t_end - t_start).count();
      oss << "error " << __func__ << ": " << std::fixed << std::setprecision(2)
          << t_total << " seconds per pass - ETA ";

      log_message = oss.str();
      OnLogMessage(log_message);
      cleasr_log_stream();
      int total_seconds = (int)(t_total * n_chunk);

      if (total_seconds >= 60 * 60) {
        oss << "error " << total_seconds / (60 * 60) << " hours ";

        log_message = oss.str();
        OnLogMessage(log_message);
        cleasr_log_stream();

        total_seconds = total_seconds % (60 * 60);
      }

      oss << "error " << total_seconds / 60 << " minutes\n"
          << "\nChunk\tPPL cumulative\tPPL chunk\n";

      log_message = oss.str();
      OnLogMessage(log_message);
      cleasr_log_stream();
    }

    // We get the logits for all the tokens in the context window (params.n_ctx)
    // from llama_eval above.  Now, based on
    // https://huggingface.co/docs/transformers/perplexity, calculate the
    // perplexity over the last half of the window (so the model always has some
    // context to predict the token).
    //
    // We rely on the fact that attention in the forward pass only looks at
    // previous tokens here, so the logits returned for each token are an
    // accurate representation of what the model would have predicted at that
    // point.
    //
    // Example, we have a context window of 512, we will compute perplexity for
    // each of the last 256 tokens.  Then, we split the input up into context
    // window size chunks to process the entire prompt.

    double nllchunk = 0.0;
    int countchunk = 0;

    for (int j = std::min(512, params.n_ctx / 2); j < params.n_ctx - 1; ++j) {
      // Calculate probability of next token, given the previous ones.
      const std::vector<float> tok_logits(logits.begin() + (j + 0) * n_vocab,
                                          logits.begin() + (j + 1) * n_vocab);

      const float prob = softmax(tok_logits)[embd_inp[start + j + 1]];

      nllchunk += -std::log(prob);
      ++countchunk;
    }

    nll += nllchunk;
    count += countchunk;

    // perplexity is e^(average negative log-likelihood)
    oss << i + 1 << "\t" << std::fixed << std::setprecision(8)
        << std::exp(nll / count) << "\t" << std::fixed << std::setprecision(8)
        << std::exp(nllchunk / countchunk) << "\n";

    log_message = oss.str();
    OnLogMessage(log_message);
    cleasr_log_stream();
  }

  // report timing
  {
    oss << "\n\n"
        << __func__ << ": mem per token = " << std::setw(8) << mem_per_token
        << " bytes\n"
        << __func__ << ": eval time = " << std::fixed << std::setprecision(2)
        << t_predict_us / 1000.0f << " ms / "
        << t_predict_us / 1000.0f / (n_chunk * params.n_ctx)
        << " ms per token\n";

    log_message = oss.str();
    OnLogMessage(log_message);
    cleasr_log_stream();
  }
}

void Mpt::OnNewTokenProcessed(const std::string &token) {}

Mpt::~Mpt() { ggml_free(model.ctx); }

void Mpt::OnLogMessage(const std::string &information) {}

Mpt::Mpt(mpt_params _params) {
  rng = std::mt19937(params.seed);

  params = _params;

  ggml_time_init();

  if (params.seed < 0) {
    params.seed = time(NULL);
  }

  if (params.n_predict < 0) {
    params.n_predict = 0;
  }

  oss << __func__ << ": seed      = " << params.seed << "\n"
      << __func__ << ": n_threads = " << params.n_threads << "\n"
      << __func__ << ": n_batch   = " << params.n_batch << "\n"
      << __func__ << ": n_ctx     = " << params.n_ctx << "\n"
      << __func__ << ": n_predict = " << params.n_predict << "\n\n";

  log_message = oss.str();
  OnLogMessage(log_message);
  cleasr_log_stream();

  int64_t t_load_us = 0;

  model.hparams.n_ctx = params.n_ctx;

  // load the model
  {
    const int64_t t_start_us = ggml_time_us();

    if (!mpt_model_load(*this, params.model, model, vocab)) {
      oss << "error " << __func__ << ": failed to load model from '"
          << params.model << "'\n";

      log_message = oss.str();
      OnLogMessage(log_message);
      cleasr_log_stream();
      return;
    }

    t_load_us = ggml_time_us() - t_start_us;
  }

  if (params.top_k == 0) {
    params.top_k = model.hparams.n_vocab;
  }

  if (params.repeat_last_n == -1) {
    params.repeat_last_n = params.n_ctx;
  }

  oss << "\n"
      << __func__ << ": temp           = " << std::fixed << std::setprecision(3)
      << params.temp << "\n"
      << __func__ << ": top_k          = " << params.top_k << "\n"
      << __func__ << ": top_p          = " << std::fixed << std::setprecision(3)
      << params.top_p << "\n"
      << __func__ << ": repeat_last_n  = " << params.repeat_last_n << "\n"
      << __func__ << ": repeat_penalty = " << std::fixed << std::setprecision(3)
      << params.repeat_penalty << "\n"
      << __func__ << ":     load time = " << std::fixed << std::setprecision(2)
      << t_load_us / 1000.0f << " ms\n";

  log_message = oss.str();
  OnLogMessage(log_message);
  cleasr_log_stream();
}

std::string Mpt::GetRandomMessage() { return gpt_random_prompt(rng); }

std::string Mpt::Process(const std::string &message) {
  std::vector<int> tokenizedMsg = TokenizeMessage(message);
  return ProcessTokenizedMessage(tokenizedMsg);
}

std::vector<int> Mpt::TokenizeMessage(const std::string &message) {
  std::vector<int> embd_inp = ::gpt_tokenize(vocab, message);

  oss << "\n";
  oss << __func__ << ": number of tokens in prompt = " << embd_inp.size()
      << "\n";

  std::string log_message = oss.str();
  OnLogMessage(log_message);
  cleasr_log_stream();

  for (size_t i = 0; i < embd_inp.size(); i++) {
    oss << __func__ << ": token[" << i << "] = " << std::setw(6) << embd_inp[i]
        << "\n";
    log_message = oss.str();
    OnLogMessage(log_message);
    cleasr_log_stream();
  }
  OnLogMessage("\n");

  return embd_inp;
}

std::string Mpt::ProcessTokenizedMessage(const std::vector<int> &embd_inp) {
  int64_t t_sample_us = 0;
  int64_t t_predict_us = 0;
  const int64_t t_main_start_us = ggml_time_us();

  std::vector<int32_t> last_n_tokens(params.n_ctx);
  std::fill(last_n_tokens.begin(), last_n_tokens.end(), 0);

  std::vector<gpt_vocab::id> embd;
  std::vector<float> logits;

  // determine the required inference memory per token:
  size_t mem_per_token = 0;
  mpt_eval(*this, model, params.n_threads, 0, {0, 1, 2, 3}, logits, false,
           mem_per_token);

  int n_past = 0;
  int n_consumed = 0;
  int n_sampled = 0;
  std::string result = "";
  while (n_sampled < params.n_predict) {
    // predict
    if (embd.size() > 0) {
      const int64_t t_start_us = ggml_time_us();

      if (!mpt_eval(*this, model, params.n_threads, n_past, embd, logits, false,
                    mem_per_token)) {
        oss << __func__ << ": failed to predict\n";

        log_message = oss.str();
        OnLogMessage(log_message);
        cleasr_log_stream();
        return "mpt_eval error";
      }

      t_predict_us += ggml_time_us() - t_start_us;

      n_past += embd.size();
      embd.clear();
    }

    if ((int)embd_inp.size() <= n_consumed) {
      // sample next token

      const int top_k = params.top_k;
      const float top_p = params.top_p;
      const float temp = params.temp;
      const int repeat_last_n = params.repeat_last_n;
      const float repeat_penalty = params.repeat_penalty;

      gpt_vocab::id id = 0;

      {
        const int64_t t_start_sample_us = ggml_time_us();

        id = gpt_sample_top_k_top_p_repeat(
            vocab, logits.data() + (logits.size() - model.hparams.n_vocab),
            last_n_tokens.data(), last_n_tokens.size(), top_k, top_p, temp,
            repeat_last_n, repeat_penalty, rng);

        last_n_tokens.erase(last_n_tokens.begin());
        last_n_tokens.push_back(id);

        t_sample_us += ggml_time_us() - t_start_sample_us;
      }

      // add it to the context
      embd.push_back(id);
      ++n_sampled;

    } else {
      // if here, it means we are still processing the input prompt
      while ((int)embd_inp.size() > n_consumed) {
        embd.push_back(embd_inp[n_consumed]);

        last_n_tokens.erase(last_n_tokens.begin());
        last_n_tokens.push_back(embd_inp[n_consumed]);

        ++n_consumed;
        if ((int)embd.size() >= params.n_batch) {
          break;
        }
      }
    }

    // display text
    for (auto id : embd) {
      auto token = vocab.id_to_token[id];
      OnNewTokenProcessed(token);
      result += token;
    }

    // end of text token
    if (embd.back() == 0) {
      break;
    }
  }

  // report timing
  {
    const int64_t t_main_end_us = ggml_time_us();

    oss << "\n\n\n"
        << __func__ << ": sampled tokens = " << std::setw(8) << n_sampled
        << "\n"
        << __func__ << ":  mem per token = " << std::setw(8) << mem_per_token
        << " bytes\n"
        << __func__ << ":    sample time = " << std::setw(8) << std::fixed
        << std::setprecision(2) << t_sample_us / 1000.0f << " ms / "
        << t_sample_us / 1000.0f / n_sampled << " ms per token\n"
        << __func__ << ":      eval time = " << std::setw(8)
        << t_predict_us / 1000.0f << " ms / " << t_predict_us / 1000.0f / n_past
        << " ms per token\n"
        << __func__ << ":     total time = " << std::setw(8)
        << (t_main_end_us - t_main_start_us) / 1000.0f << " ms\n";

    log_message = oss.str();
    OnLogMessage(log_message);
    cleasr_log_stream();
  }
  return result;
}