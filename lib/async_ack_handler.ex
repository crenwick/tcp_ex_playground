defmodule AsyncAck.Handler do
  use GenServer

  def start_link(ref, socket, transport, opts) do
    pid = :proc_lib.spawn_link(__MODULE__, :init, [ref, socket, transport, opts])

    {:ok, pid}
  end

  @doc """
  Needed so that elixir doesn't yell at compile time
  """
  def init(init_arg) do
    {:ok, init_arg}
  end

  def init(ref, socket, transport, _Opts = []) do
    :ok = :ranch.accept_ack(ref)
    :ok = transport.setopts(socket, active: true, nodelay: true)

    responder_pid = spawn_link(__MODULE__, :responder, [socket, transport, <<>>, [], 0])
    Process.flag(:trap_exit, true)

    :gen_server.enter_loop(__MODULE__, [], %{
      socket: socket,
      transport: transport,
      responder_pid: responder_pid
    })
  end

  def calc_skipped([]) do
    0
  end

  def calc_skipped([{_, skipped}]) do
    skipped
  end

  def calc_skipped([{_, skipped} | rest]) do
    1 + skipped + calc_skipped(rest)
  end

  def flush(_, _, []) do
  end

  def flush(socket, transport, ack_list) do
    [{id, _} | _] = ack_list
    skipped = calc_skipped(ack_list)
    packet = <<id::binary-size(8), skipped::little-size(32)>>
    transport.send(socket, packet)
  end

  def responder(socket, transport, yet_to_parse, ack_list, packet_count) do
    receive do
      {:message, packet} ->
        case parse(yet_to_parse <> packet, <<>>, 0) do
          {not_yet_parsed, {id, skipped}} ->
            new_ack_list = [{id, skipped} | ack_list]

            if packet_count > 20 do
              flush(socket, transport, new_ack_list)
              responder(socket, transport, not_yet_parsed, [], 0)
            else
              responder(socket, transport, not_yet_parsed, new_ack_list, packet_count + 1)
            end

          {not_yet_parsed, {}} ->
            responder(socket, transport, not_yet_parsed, ack_list, packet_count + 1)
        end

      {:stop} ->
        :stop
    after
      5 ->
        flush(socket, transport, ack_list)
        responder(socket, transport, yet_to_parse, [], 0)
    end
  end

  # Server callbacks

  def handle_info({:tcp, _, packet}, state) do
    {:message_queue_len, length} = :erlang.process_info(state.responder_pid, :message_queue_len)

    if(length > 100) do
      :timer.sleep(div(length, 100))
    end

    send(state.responder_pid, {:message, packet})

    {:noreply, state}
  end

  def handle_info({:tcp_closed, _}, state) do
    shutdown(state.socket, state.transport, state.responder_pid)
  end

  def handle_info({:tcp_error, _, _reason}, state) do
    shutdown(state.socket, state.transport, state.responder_pid)
  end

  defp shutdown(socket, transport, responder_pid) do
    send(responder_pid, {:stop})

    receive do
      {:EXIT, _responder_pid, :normal} -> :ok
    end

    :ok = transport.close(socket)

    {:stop, :normal, %{}}
  end

  defp parse(<<>>, <<>>, _skipped) do
    {<<>>, {}}
  end

  defp parse(<<>>, last_id, skipped) do
    {<<>>, {last_id, skipped}}
  end

  defp parse(packet, <<>>, 0) do
    case packet do
      # TODO : revise this 1MB safeguard against garbage here
      <<id::binary-size(8), sz::little-size(32), _data::binary-size(sz)>> when sz < 1_000_000 ->
        {<<>>, {id, 0}}

      <<id::binary-size(8), sz::little-size(32), _data::binary-size(sz), rest::binary>>
      when sz < 100 ->
        parse(rest, id, 0)

      unparsed ->
        {unparsed, {}}
    end
  end

  defp parse(packet, last_id, skipped) do
    case packet do
      # TODO : revise this 1MB safeguard against garbage here
      <<id::binary-size(8), sz::little-size(32), _data::binary-size(sz)>> when sz < 1_000_000 ->
        {<<>>, {id, skipped + 1}}

      <<id::binary-size(8), sz::little-size(32), _data::binary-size(sz), rest::binary>>
      when sz < 100 ->
        parse(rest, id, skipped + 1)

      unparsed ->
        {unparsed, {last_id, skipped}}
    end
  end
end
