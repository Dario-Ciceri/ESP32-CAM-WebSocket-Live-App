import 'dart:async';
import 'dart:convert';

import 'package:app_settings/app_settings.dart';
import 'package:connectivity_plus/connectivity_plus.dart';
import 'package:crypto/crypto.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:multicast_dns/multicast_dns.dart';
import 'package:shared_preferences/shared_preferences.dart';
import 'package:web_socket_channel/web_socket_channel.dart';

void main() => runApp(MyApp());

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'ESP32-CAM Stream',
      theme: ThemeData.dark(),
      home: StreamPage(),
    );
  }
}

class StreamPage extends StatefulWidget {
  const StreamPage({super.key});

  @override
  State<StreamPage> createState() => StreamPageState();
}

class StreamPageState extends State<StreamPage> {
  bool _isConnected = false;
  bool _isSearching = false;
  WebSocketChannel? _channel;
  String? _deviceAddress;
  String _token = '';
  final TextEditingController _tokenController = TextEditingController();
  Timer? _reconnectTimer;
  final Connectivity _connectivity = Connectivity();
  StreamSubscription? _connectivitySubscription;

  // Definisci il nome del servizio mDNS per ESP32-CAM
  final String _serviceName = '_esp32cam._tcp.local';
  // Nome del dispositivo mdns
  final String _deviceName = 'ESP32-CAM';

  @override
  void initState() {
    super.initState();
    _loadSavedToken();
    _initConnectivity();
  }

  Future<void> _loadSavedToken() async {
    final prefs = await SharedPreferences.getInstance();
    setState(() {
      _token = prefs.getString('auth_token') ?? '';
      _tokenController.text = _token;
    });
  }

  Future<void> _saveToken(String token) async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.setString('auth_token', token);
    setState(() {
      _token = token;
    });
  }

  Future<void> _initConnectivity() async {
    _connectivitySubscription = _connectivity.onConnectivityChanged.listen(
      (event) {
        _updateConnectionStatus(event.first);
      },
    );

    // Verifica lo stato di connessione attuale
    final result = await _connectivity.checkConnectivity();
    _updateConnectionStatus(result.first);
  }

  void _updateConnectionStatus(ConnectivityResult result) {
    if (result == ConnectivityResult.wifi) {
      // Se siamo connessi a una rete WiFi, cerca il dispositivo
      _findDeviceWithMDNS();
    } else {
      // Se la connessione è persa, disconnetti dal WebSocket
      _disconnect();
      setState(() {
        _isConnected = false;
        _deviceAddress = null;
      });
    }
  }

  void _disconnect() {
    _channel?.sink.close();
    _channel = null;
    _reconnectTimer?.cancel();
  }

  Future<void> _findDeviceWithMDNS() async {
    if (_isSearching) return;

    setState(() {
      _isSearching = true;
    });

    try {
      final MDnsClient client = MDnsClient();
      await client.start();

      debugPrint('Ricerca dispositivo: $_serviceName');

      // Cerca servizi ESP32-CAM sulla rete
      await for (final PtrResourceRecord ptr
          in client.lookup<PtrResourceRecord>(
              ResourceRecordQuery.serverPointer(_serviceName))) {
        await for (final SrvResourceRecord srv
            in client.lookup<SrvResourceRecord>(
                ResourceRecordQuery.service(ptr.domainName))) {
          await for (final IPAddressResourceRecord ip
              in client.lookup<IPAddressResourceRecord>(
                  ResourceRecordQuery.addressIPv4(srv.target))) {
            debugPrint(
                'Trovato dispositivo: ${srv.target} all\'indirizzo: ${ip.address.address}:${srv.port}');

            setState(() {
              _deviceAddress = '${ip.address.address}:${srv.port}';
              _isSearching = false;
            });

            client.stop();

            if (_token.isNotEmpty) {
              _connectWebSocket();
            }

            return;
          }
        }
      }

      client.stop();
      setState(() {
        _isSearching = false;
      });

      // Se non troviamo il dispositivo, mostra un messaggio
      _showMessage('Dispositivo ESP32-CAM non trovato nella rete');
    } catch (e) {
      debugPrint('Errore nella ricerca mDNS: $e');
      setState(() {
        _isSearching = false;
      });
      _showMessage('Errore nella ricerca mDNS: $e');
    }
  }

  void _connectWebSocket() {
    if (_deviceAddress == null || _token.isEmpty) {
      return;
    }

    _disconnect();

    try {
      // Genera un hash del token per l'autenticazione
      final hashedToken = sha256.convert(utf8.encode(_token)).toString();

      debugPrint(
          'Connessione a ws://$_deviceAddress/stream?token=$hashedToken');

      _channel = WebSocketChannel.connect(
        Uri.parse('ws://$_deviceAddress/stream?token=$hashedToken'),
      );

      // Verifica lo stato della connessione dopo alcuni secondi
      Future.delayed(Duration(seconds: 3), () {
        if (_channel != null) {
          setState(() => _isConnected = true);
        }
      });

      // Imposta un timer di riconnessione in caso di problemi
      _reconnectTimer = Timer.periodic(Duration(seconds: 30), (timer) {
        if (_channel == null && _deviceAddress != null) {
          _connectWebSocket();
        }
      });
    } catch (e) {
      debugPrint('Errore di connessione WebSocket: $e');
      _showConnectionError();
    }
  }

  void _showConnectionError() {
    _showMessage('Impossibile connettersi allo stream');
  }

  void _showMessage(String message) {
    if (!mounted) return;

    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(message),
        duration: Duration(seconds: 3),
      ),
    );
  }

  @override
  void dispose() {
    _channel?.sink.close();
    _reconnectTimer?.cancel();
    _connectivitySubscription?.cancel();
    _tokenController.dispose();
    super.dispose();
  }

  void _showTokenDialog() {
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Imposta Token di Autenticazione'),
        content: TextField(
          controller: _tokenController,
          decoration: InputDecoration(
            labelText: 'Token di Sicurezza',
            hintText: 'Inserisci il token per ESP32-CAM',
          ),
          obscureText: true,
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(context),
            child: Text('Annulla'),
          ),
          ElevatedButton(
            onPressed: () {
              _saveToken(_tokenController.text);
              Navigator.pop(context);
              if (_deviceAddress != null) {
                _connectWebSocket();
              } else {
                _findDeviceWithMDNS();
              }
            },
            child: Text('Salva'),
          ),
        ],
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: Text('ESP32-CAM Live Stream'),
        actions: [
          IconButton(
            icon: Icon(Icons.refresh),
            onPressed: _findDeviceWithMDNS,
            tooltip: 'Ricerca dispositivo',
          ),
          IconButton(
            icon: Icon(Icons.vpn_key),
            onPressed: _showTokenDialog,
            tooltip: 'Imposta token',
          ),
          IconButton(
            icon: Icon(Icons.settings),
            onPressed: () =>
                AppSettings.openAppSettings(type: AppSettingsType.wifi),
            tooltip: 'Impostazioni WiFi',
          ),
        ],
      ),
      body: _buildBody(),
    );
  }

  Widget _buildBody() {
    if (_isSearching) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            CircularProgressIndicator(),
            SizedBox(height: 16),
            Text('Ricerca del dispositivo ESP32-CAM in corso...'),
          ],
        ),
      );
    }

    if (_deviceAddress == null) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Text('Dispositivo ESP32-CAM non trovato'),
            SizedBox(height: 16),
            ElevatedButton(
              onPressed: _findDeviceWithMDNS,
              child: Text('Cerca dispositivo'),
            ),
          ],
        ),
      );
    }

    if (_token.isEmpty) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Text('Token di autenticazione richiesto'),
            SizedBox(height: 16),
            ElevatedButton(
              onPressed: _showTokenDialog,
              child: Text('Imposta Token'),
            ),
          ],
        ),
      );
    }

    if (!_isConnected) {
      return Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            CircularProgressIndicator(),
            SizedBox(height: 16),
            Text('Connessione al dispositivo $_deviceName...'),
            Text('Indirizzo: $_deviceAddress'),
          ],
        ),
      );
    }

    return StreamBuilder(
      stream: _channel?.stream,
      builder: (context, snapshot) {
        if (snapshot.hasError) {
          return Center(
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                Text('Errore nello stream: ${snapshot.error}'),
                SizedBox(height: 16),
                ElevatedButton(
                  onPressed: _connectWebSocket,
                  child: Text('Riprova'),
                ),
              ],
            ),
          );
        }

        if (snapshot.connectionState == ConnectionState.waiting) {
          return Center(child: CircularProgressIndicator());
        }

        if (snapshot.hasData && snapshot.data is Uint8List) {
          return InteractiveViewer(
            panEnabled: true,
            maxScale: 4.0,
            child: Center(
              child: Image.memory(
                snapshot.data as Uint8List,
                gaplessPlayback: true,
                filterQuality: FilterQuality.high,
                fit: BoxFit.contain,
              ),
            ),
          );
        }

        return Center(child: Text('In attesa del flusso video...'));
      },
    );
  }
}
