#include "stream.hh"

#ifdef DEBUG_STREAM
#  undef LOG
#  define LOG(...)
#endif

namespace pulse {
#define LOG_BA(ba) LOG("buffer_attr{fragsize=%d,maxlength=%d,minreq=%d,prebuf=%d,tlength=%d}", ba.fragsize, ba.maxlength, ba.minreq, ba.prebuf, ba.tlength);
  
  Stream::Stream(Context& context,
                 String::Utf8Value *stream_name,
                 const pa_sample_spec *sample_spec,
                 pa_usec_t initial_latency):
    ctx(context), latency(initial_latency), write_offset(0) {
    
    ctx.Ref();
    
    pa_ss = *sample_spec;
    
    pa_stm = pa_stream_new(ctx.pa_ctx, stream_name ? **stream_name : "node-stream", &pa_ss, NULL);
    
    buffer_attr.fragsize = (uint32_t)-1;
    buffer_attr.maxlength = (uint32_t)-1;
    buffer_attr.minreq = (uint32_t)-1;
    buffer_attr.prebuf = (uint32_t)-1;
    buffer_attr.tlength = (uint32_t)-1;
    
    pa_stream_set_state_callback(pa_stm, StateCallback, this);
    
    pa_stream_set_buffer_attr_callback(pa_stm, BufferAttrCallback, this);
    pa_stream_set_latency_update_callback(pa_stm, LatencyCallback, this);
  }
  
  Stream::~Stream(){
    if(pa_stm){
      disconnect();
      pa_stream_unref(pa_stm);
    }
    ctx.Unref();
  }
  
  void Stream::StateCallback(pa_stream *s, void *ud){
    Stream *stm = static_cast<Stream*>(ud);
    
    stm->pa_state = pa_stream_get_state(stm->pa_stm);
    
    if(!stm->state_callback.IsEmpty()){
      TryCatch try_catch;
      
      Handle<Value> args[] = {
        Number::New(stm->pa_state),
        Undefined()
      };

      if(stm->pa_state == PA_STREAM_FAILED){
        args[1] = EXCEPTION(Error, pa_strerror(pa_context_errno(stm->ctx.pa_ctx)));
      }
      
      stm->state_callback->Call(stm->handle_, 2, args);

      HANDLE_CAUGHT(try_catch);
    }
  }
  
  void Stream::state_listener(Handle<Value> callback){
    if(callback->IsFunction()){
      state_callback = Persistent<Function>::New(Handle<Function>::Cast(callback));
    }else{
      state_callback.Dispose();
    }
  }
  
  void Stream::BufferAttrCallback(pa_stream *s, void *ud){
    Stream *stm = static_cast<Stream*>(ud);
    
    LOG_BA(stm->buffer_attr);
  }

  void Stream::LatencyCallback(pa_stream *s, void *ud){
    Stream *stm = static_cast<Stream*>(ud);
    
    pa_usec_t usec;
    int neg;
    
    pa_stream_get_latency(stm->pa_stm, &usec, &neg);
    
    LOG("latency %s%8d us", neg ? "-" : "", (int)usec);
    
    stm->latency = usec;
  }
  
  int Stream::connect(String::Utf8Value *device_name, pa_stream_direction_t direction, pa_stream_flags_t flags){
    switch(direction){
    case PA_STREAM_PLAYBACK: {
      if(latency){
        buffer_attr.tlength = pa_usec_to_bytes(latency, &pa_ss);
      }
      
      LOG_BA(buffer_attr);
      
      pa_stream_set_write_callback(pa_stm, RequestCallback, this);
      pa_stream_set_underflow_callback(pa_stm, UnderflowCallback, this);
      
      return pa_stream_connect_playback(pa_stm, device_name ? **device_name : NULL, &buffer_attr, flags, NULL, NULL);
    }
    case PA_STREAM_RECORD: {
      if(latency){
        buffer_attr.fragsize = pa_usec_to_bytes(latency, &pa_ss);
      }
      
      LOG_BA(buffer_attr);
      
      pa_stream_set_read_callback(pa_stm, ReadCallback, this);
      
      return pa_stream_connect_record(pa_stm, device_name ? **device_name : NULL, &buffer_attr, flags);
    }
    case PA_STREAM_UPLOAD: {
      return pa_stream_connect_upload(pa_stm, 0);
    }
    case PA_STREAM_NODIRECTION:
      break;
    }
    return 0;
  }
  
  void Stream::disconnect(){
    pa_stream_disconnect(pa_stm);
  }

  void Stream::ReadCallback(pa_stream *s, size_t nb, void *ud){
    if(nb > 0){
      Stream *stm = static_cast<Stream*>(ud);
      
      stm->data();
    }
  }
  
  void Stream::data(){
    if(read_callback.IsEmpty()){
      return;
    }
    
    const void *data = NULL;
    size_t size;
    
    pa_stream_peek(pa_stm, &data, &size);
    LOG("Stream::read callback %d", (int)size);
    pa_stream_drop(pa_stm);
    
    Buffer *buffer = Buffer::New((const char*)data, size);
    
    TryCatch try_catch;
    
    Handle<Value> args[] = {
      buffer->handle_
    };
    
    read_callback->Call(handle_, 1, args);
    
    HANDLE_CAUGHT(try_catch);
  }
  
  void Stream::read(Handle<Value> callback){
    if(callback->IsFunction()){
      pa_stream_drop(pa_stm);
      read_callback = Persistent<Function>::New(Handle<Function>::Cast(callback));
      //pa_stream_drop(pa_stm);
      //pa_stream_flush(pa_stm, NULL, NULL);
      pa_stream_cork(pa_stm, 0, NULL, NULL);
    }else{
      pa_stream_cork(pa_stm, 1, NULL, NULL);
      pa_stream_drop(pa_stm);
      //pa_stream_flush(pa_stm, NULL, NULL);
      read_callback.Dispose();
      read_callback.Clear();
    }
  }
  
  /* write */
  
  void Stream::DrainCallback(pa_stream *s, int st, void *ud){
    Stream *stm = static_cast<Stream*>(ud);
    
    stm->drain();
  }

  void Stream::drain(){
    LOG("Stream::drain");

    if(!write_buffer.IsEmpty()){
      //LOG("Stream::drain buffer del");
      write_buffer.Dispose();
      write_buffer.Clear();
    }
    
    if(!drain_callback.IsEmpty()){
      TryCatch try_catch;
      
      Handle<Value> args[0];
      
      Handle<Function> callback = drain_callback;
      
      //LOG("Stream::drain callback del");
      drain_callback.Dispose();
      drain_callback.Clear();
      
      //LOG("Stream::drain callback call");
      callback->Call(handle_, 0, args);

      HANDLE_CAUGHT(try_catch);
    }
  }

  static void DummyFree(void *p){}

  void Stream::RequestCallback(pa_stream *s, size_t length, void *ud){
    Stream *stm = static_cast<Stream*>(ud);
    
    if(stm->request(length) < length){
      stm->drain();
    }
  }

  size_t Stream::request(size_t length){
    if(write_buffer.IsEmpty()){
      return length;
    }
    
    size_t end_length = Buffer::Length(write_buffer) - write_offset;
    size_t write_length = length;
    
    if(!end_length){
      return 0;
    }
    
    if(write_length > end_length){
      write_length = end_length;
    }
    
    LOG("write req=%d offset=%d chunk=%d", length, write_offset, write_length);
    
    pa_stream_write(pa_stm, ((const char*)Buffer::Data(write_buffer)) + write_offset, write_length, DummyFree, 0, PA_SEEK_RELATIVE);
    
    write_offset += write_length;
    
    return write_length;
  }
  
  void Stream::UnderflowCallback(pa_stream *s, void *ud){
    Stream *stm = static_cast<Stream*>(ud);
    
    stm->underflow();
  }

  void Stream::underflow(){
    LOG("underflow");
    
  }
  
  void Stream::write(Handle<Value> buffer, Handle<Value> callback){
    if(!write_buffer.IsEmpty()){
      //LOG("Stream::write flush");
      pa_stream_flush(pa_stm, DrainCallback, this);
    }
    
    if(callback->IsFunction()){
      //LOG("Stream::write callback add");
      drain_callback = Persistent<Function>::New(Handle<Function>::Cast(callback));
    }
    
    if(Buffer::HasInstance(buffer)){
      //LOG("Stream::write buffer add");
      write_buffer = Persistent<Value>::New(buffer);
      write_offset = 0;
      LOG("Stream::write");
      
      if(pa_stream_is_corked(pa_stm)){
        pa_stream_cork(pa_stm, 0, NULL, NULL);
      }
      
      size_t length = pa_stream_writable_size(pa_stm);
      if(length > 0){
        if(request(length) < length){
          drain();
        }
      }
    }else{
      pa_stream_cork(pa_stm, 1, NULL, NULL);
    }
  }
  
  /* bindings */

  void
  Stream::Init(Handle<Object> target){
    Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
    
    tpl->SetClassName(String::NewSymbol("PulseAudioStream"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    
    SetPrototypeMethod(tpl, "connect", Connect);
    SetPrototypeMethod(tpl, "disconnect", Disconnect);
    
    SetPrototypeMethod(tpl, "latency", Latency);
    SetPrototypeMethod(tpl, "read", Read);
    SetPrototypeMethod(tpl, "write", Write);
    
    Local<Function> cfn = tpl->GetFunction();
    
    target->Set(String::NewSymbol("Stream"), cfn);
    
    AddEmptyObject(cfn, type);
    DefineConstant(type, playback, PA_STREAM_PLAYBACK);
    DefineConstant(type, record, PA_STREAM_RECORD);
    DefineConstant(type, upload, PA_STREAM_UPLOAD);

    AddEmptyObject(cfn, format);
    DefineConstant(format, U8, PA_SAMPLE_U8);
    DefineConstant(format, S16LE, PA_SAMPLE_S16LE);
    DefineConstant(format, S16BE, PA_SAMPLE_S16BE);
    DefineConstant(format, F32LE, PA_SAMPLE_FLOAT32LE);
    DefineConstant(format, F32BE, PA_SAMPLE_FLOAT32BE);
    DefineConstant(format, ALAW, PA_SAMPLE_ALAW);
    DefineConstant(format, ULAW, PA_SAMPLE_ULAW);
    DefineConstant(format, S32LE, PA_SAMPLE_S32LE);
    DefineConstant(format, S32BE, PA_SAMPLE_S32BE);
    DefineConstant(format, S24LE, PA_SAMPLE_S24LE);
    DefineConstant(format, S24BE, PA_SAMPLE_S24BE);
    DefineConstant(format, S24_32LE, PA_SAMPLE_S24_32LE);
    DefineConstant(format, S24_32BE, PA_SAMPLE_S24_32BE);

    AddEmptyObject(cfn, flags);
    DefineConstant(flags, noflags, PA_STREAM_NOFLAGS);
    DefineConstant(flags, start_corked, PA_STREAM_START_CORKED);
    DefineConstant(flags, interpolate_timing, PA_STREAM_INTERPOLATE_TIMING);
    DefineConstant(flags, not_monotonic, PA_STREAM_NOT_MONOTONIC);
    DefineConstant(flags, auto_timing_update, PA_STREAM_AUTO_TIMING_UPDATE);
    DefineConstant(flags, no_remap_channels, PA_STREAM_NO_REMAP_CHANNELS);
    DefineConstant(flags, no_remix_channels, PA_STREAM_NO_REMIX_CHANNELS);
    DefineConstant(flags, fix_format, PA_STREAM_FIX_FORMAT);
    DefineConstant(flags, fix_rate, PA_STREAM_FIX_RATE);
    DefineConstant(flags, fix_channels, PA_STREAM_FIX_CHANNELS);
    DefineConstant(flags, dont_move, PA_STREAM_DONT_MOVE);
    DefineConstant(flags, variable_rate, PA_STREAM_VARIABLE_RATE);
    DefineConstant(flags, peak_detect, PA_STREAM_PEAK_DETECT);
    DefineConstant(flags, start_muted, PA_STREAM_START_MUTED);
    DefineConstant(flags, adjust_latency, PA_STREAM_ADJUST_LATENCY);
    DefineConstant(flags, early_requests, PA_STREAM_EARLY_REQUESTS);
    DefineConstant(flags, dont_inhibit_auto_suspend, PA_STREAM_DONT_INHIBIT_AUTO_SUSPEND);
    DefineConstant(flags, start_unmuted, PA_STREAM_START_UNMUTED);
    DefineConstant(flags, fail_on_suspend, PA_STREAM_FAIL_ON_SUSPEND);
    DefineConstant(flags, relative_volume, PA_STREAM_RELATIVE_VOLUME);
    DefineConstant(flags, passthrough, PA_STREAM_PASSTHROUGH);
    
    AddEmptyObject(cfn, state);
    DefineConstant(state, unconnected, PA_STREAM_UNCONNECTED);
    DefineConstant(state, creating, PA_STREAM_CREATING);
    DefineConstant(state, ready, PA_STREAM_READY);
    DefineConstant(state, failed, PA_STREAM_FAILED);
    DefineConstant(state, terminated, PA_STREAM_TERMINATED);
  }

  Handle<Value>
  Stream::New(const Arguments& args){
    HandleScope scope;

    JS_ASSERT(args.IsConstructCall());
    
    JS_ASSERT(args.Length() == 7);
    JS_ASSERT(args[0]->IsObject());
    
    Context *ctx = ObjectWrap::Unwrap<Context>(args[0]->ToObject());
    
    JS_ASSERT(ctx);
    
    pa_sample_spec ss;
    
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 44100;
    ss.channels = 2;
    
    if(args[1]->IsUint32()){
      ss.format = pa_sample_format_t(args[1]->Uint32Value());
    }
    if(args[2]->IsUint32()){
      ss.rate = args[2]->Uint32Value();
    }
    if(args[3]->IsUint32()){
      ss.channels = uint8_t(args[3]->Uint32Value());
    }
    
    pa_usec_t latency = 0;
    if(args[4]->IsUint32()){
      latency = pa_usec_t(args[4]->Uint32Value());
    }
    
    String::Utf8Value *stream_name = NULL;
    if(args[5]->IsString()){
      stream_name = new String::Utf8Value(args[5]->ToString());
    }
    
    /* initialize instance */
    Stream *stm = new Stream(*ctx, stream_name, &ss, latency);
    
    if(stream_name){
      delete stream_name;
    }
    
    if(!stm->pa_stm){
      delete stm;
      THROW_SCOPE(Error, "Unable to create stream.");
    }
    stm->Wrap(args.This());
    
    if(args[6]->IsFunction()){
      stm->state_listener(args[6]);
    }
    
    return scope.Close(args.This());
  }

  Handle<Value>
  Stream::Connect(const Arguments& args){
    HandleScope scope;

    JS_ASSERT(args.Length() == 3);
    
    Stream *stm = ObjectWrap::Unwrap<Stream>(args.This());
    
    JS_ASSERT(stm);
    
    String::Utf8Value *device_name = NULL;
    if(args[0]->IsString()){
      device_name = new String::Utf8Value(args[0]->ToString());
    }
    
    pa_stream_direction_t sd = PA_STREAM_PLAYBACK;
    if(args[1]->IsUint32()){
      sd = pa_stream_direction_t(args[1]->Uint32Value());
    }

    pa_stream_flags_t sf = PA_STREAM_NOFLAGS;
    if(args[2]->IsUint32()){
      sf = pa_stream_flags_t(args[2]->Uint32Value());
    }
    
    int status = stm->connect(device_name, sd, sf);
    if(device_name){
      delete device_name;
    }
    PA_ASSERT(status);
    
    return scope.Close(Undefined());
  }

  Handle<Value>
  Stream::Disconnect(const Arguments& args){
    HandleScope scope;
    
    Stream *stm = ObjectWrap::Unwrap<Stream>(args.This());
    
    JS_ASSERT(stm);
    
    stm->disconnect();
    
    return scope.Close(Undefined());
  }
  
  Handle<Value>
  Stream::Latency(const Arguments& args){
    HandleScope scope;
    
    Stream *stm = ObjectWrap::Unwrap<Stream>(args.This());
    
    JS_ASSERT(stm);
    
    pa_usec_t latency;
    int negative;
    
    PA_ASSERT(pa_stream_get_latency(stm->pa_stm, &latency, &negative));
    
    return scope.Close(Number::New(latency));
  }
  
  Handle<Value>
  Stream::Read(const Arguments& args){
    HandleScope scope;

    Stream *stm = ObjectWrap::Unwrap<Stream>(args.This());
    
    JS_ASSERT(stm);
    JS_ASSERT(args.Length() == 1);

    stm->read(args[0]);
    
    return scope.Close(Undefined());
  }
  
  Handle<Value>
  Stream::Write(const Arguments& args){
    HandleScope scope;
    
    Stream *stm = ObjectWrap::Unwrap<Stream>(args.This());
    
    JS_ASSERT(stm);
    JS_ASSERT(args.Length() == 2);
    
    stm->write(args[0], args[1]);
    
    return scope.Close(Undefined());
  }
}
