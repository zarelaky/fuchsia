// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::constants::{LOCAL_SOCAT, MAX_RETRY_COUNT, SOCAT, SOCKET, TARGET_SOCAT},
    crate::discovery::{TargetFinder, TargetFinderConfig},
    crate::mdns::MdnsTargetFinder,
    crate::target::{Target, TargetCollection},
    anyhow::{Context, Error},
    ascendd_lib::run_ascendd,
    fidl::endpoints::{ClientEnd, RequestStream, ServiceMarker},
    fidl_fidl_developer_bridge::{DaemonMarker, DaemonRequest, DaemonRequestStream},
    fidl_fuchsia_developer_remotecontrol::{RemoteControlMarker, RemoteControlProxy},
    fidl_fuchsia_overnet::{
        ServiceConsumerProxyInterface, ServiceProviderRequest, ServiceProviderRequestStream,
    },
    fidl_fuchsia_overnet_protocol::NodeId,
    fidl_fuchsia_test_manager as ftest_manager,
    futures::channel::mpsc,
    futures::lock::Mutex,
    futures::prelude::*,
    hoist::spawn,
    std::process::Command,
    std::sync::Arc,
    std::time::Duration,
};

mod constants;
mod discovery;
mod mdns;
mod net;
mod target;

async fn start_ascendd() {
    log::info!("Starting ascendd");
    spawn(async move {
        run_ascendd(ascendd_lib::Opt { sockpath: Some(SOCKET.to_string()), ..Default::default() })
            .await
            .unwrap();
    });
    log::info!("Connecting to target");
    Command::new(SOCAT).arg(LOCAL_SOCAT).arg(TARGET_SOCAT).spawn().unwrap();
}

// Daemon
#[derive(Clone)]
pub struct Daemon {
    remote_control_proxy: RemoteControlProxy,
    target_collection: Arc<Mutex<TargetCollection>>,
}

impl Daemon {
    pub async fn new() -> Result<Daemon, Error> {
        let (tx, rx) = mpsc::unbounded::<Target>();
        let target_collection = Arc::new(Mutex::new(TargetCollection::new()));
        Daemon::spawn_receiver_loop(rx, Arc::clone(&target_collection));
        let config =
            TargetFinderConfig { broadcast_interval: Duration::from_secs(120), mdns_ttl: 255 };
        let mdns = MdnsTargetFinder::new(&config)?;
        mdns.start(&tx)?;

        let mut peer_id = Daemon::find_remote_control().await?;
        let remote_control_proxy = Daemon::create_remote_control_proxy(&mut peer_id).await?;
        Ok(Daemon { remote_control_proxy, target_collection })
    }

    pub fn spawn_receiver_loop(
        mut rx: mpsc::UnboundedReceiver<Target>,
        tc: Arc<Mutex<TargetCollection>>,
    ) {
        spawn(async move {
            loop {
                let target = rx.next().await.unwrap();
                let mut targets = tc.lock().await;
                targets.merge_insert(target);
            }
        });
    }

    pub fn new_with_proxy(remote_control_proxy: RemoteControlProxy) -> Daemon {
        Daemon {
            remote_control_proxy,
            target_collection: Arc::new(Mutex::new(TargetCollection::new())),
        }
    }

    pub async fn handle_requests_from_stream(
        &self,
        mut stream: DaemonRequestStream,
        quiet: bool,
    ) -> Result<(), Error> {
        while let Some(req) = stream.try_next().await? {
            self.handle_request(req, quiet).await?;
        }
        Ok(())
    }

    async fn create_remote_control_proxy(id: &mut NodeId) -> Result<RemoteControlProxy, Error> {
        let svc = hoist::connect_as_service_consumer()?;
        let (s, p) = fidl::Channel::create().context("failed to create zx channel")?;
        svc.connect_to_service(id, RemoteControlMarker::NAME, s)?;
        let proxy = fidl::AsyncChannel::from_channel(p).context("failed to make async channel")?;
        Ok(RemoteControlProxy::new(proxy))
    }

    async fn find_remote_control() -> Result<NodeId, Error> {
        let svc = hoist::connect_as_service_consumer()?;
        // Sometimes list_peers doesn't properly report the published services - retry a few times
        // but don't loop indefinitely.
        for _ in 0..MAX_RETRY_COUNT {
            let peers = svc.list_peers().await?;
            for peer in peers {
                if peer.description.services.is_none() {
                    continue;
                }
                if peer
                    .description
                    .services
                    .unwrap()
                    .iter()
                    .find(|name| *name == RemoteControlMarker::NAME)
                    .is_none()
                {
                    continue;
                }
                return Ok(peer.id);
            }
        }
        panic!("No remote control found.  Is a device connected?")
    }

    pub async fn handle_request(&self, req: DaemonRequest, quiet: bool) -> Result<(), Error> {
        match req {
            DaemonRequest::EchoString { value, responder } => {
                if !quiet {
                    log::info!("Received echo request for string {:?}", value);
                }
                responder.send(value.as_ref()).context("error sending response")?;
                if !quiet {
                    log::info!("echo response sent successfully");
                }
            }
            DaemonRequest::StartComponent {
                component_url,
                args,
                component_stdout: stdout,
                component_stderr: stderr,
                controller,
                responder,
            } => {
                if !quiet {
                    log::info!(
                        "Received run component request for string {:?}:{:?}",
                        component_url,
                        args
                    );
                }

                let mut response = self
                    .remote_control_proxy
                    .start_component(
                        &component_url,
                        &mut args.iter().map(|s| s.as_str()),
                        stdout,
                        stderr,
                        controller,
                    )
                    .await?;
                responder.send(&mut response).context("error sending response")?;
            }
            DaemonRequest::ListTargets { value, responder } => {
                if !quiet {
                    log::info!("Received list target request for '{:?}'", value);
                }
                // TODO(awdavies): Make this into a common format for easy
                // parsing.
                let targets = self.target_collection.lock().await;
                let response = match value.as_ref() {
                    "" => format!("{:?}", targets),
                    _ => format!("{:?}", targets.target_by_nodename(&value)),
                };
                responder.send(response.as_ref()).context("error sending response")?;
            }
            DaemonRequest::LaunchSuite { test_url, suite, controller, responder } => {
                if !quiet {
                    log::info!("Received launch suite request for '{:?}'", test_url);
                }
                match self.remote_control_proxy.launch_suite(&test_url, suite, controller).await {
                    Ok(mut r) => responder.send(&mut r).context("sending LaunchSuite response")?,
                    Err(_) => responder
                        .send(&mut Err(ftest_manager::LaunchError::InternalError))
                        .context("sending LaunchSuite error")?,
                }
            }
            _ => {
                log::info!("Unsupported method");
            }
        }
        Ok(())
    }
}

////////////////////////////////////////////////////////////////////////////////
// Overnet Server implementation

async fn next_request(
    stream: &mut ServiceProviderRequestStream,
) -> Result<Option<ServiceProviderRequest>, Error> {
    Ok(stream.try_next().await.context("error running service provider server")?)
}

async fn exec_server(daemon: Daemon, quiet: bool) -> Result<(), Error> {
    let (s, p) = fidl::Channel::create().context("failed to create zx channel")?;
    let chan = fidl::AsyncChannel::from_channel(s).context("failed to make async channel")?;
    let mut stream = ServiceProviderRequestStream::from_channel(chan);
    hoist::publish_service(DaemonMarker::NAME, ClientEnd::new(p))?;
    while let Some(ServiceProviderRequest::ConnectToService {
        chan,
        info: _,
        control_handle: _control_handle,
    }) = next_request(&mut stream).await?
    {
        if !quiet {
            log::trace!("Received service request for service");
        }
        let chan =
            fidl::AsyncChannel::from_channel(chan).context("failed to make async channel")?;
        let daemon_clone = daemon.clone();
        spawn(async move {
            daemon_clone
                .handle_requests_from_stream(DaemonRequestStream::from_channel(chan), quiet)
                .await
                .unwrap_or_else(|err| panic!("fatal error handling request: {:?}", err));
        });
    }
    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// start

pub fn is_daemon_running() -> bool {
    // Try to connect directly to the socket. This will fail if nothing is listening on the other side
    // (even if the path exists).
    match std::os::unix::net::UnixStream::connect(SOCKET) {
        Ok(_) => true,
        Err(_) => false,
    }
}

pub async fn start() -> Result<(), Error> {
    if is_daemon_running() {
        return Ok(());
    }
    log::info!("Starting ascendd");
    start_ascendd().await;
    log::info!("Starting daemon overnet server");
    let daemon = Daemon::new().await?;

    exec_server(daemon, true).await
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use fidl::endpoints::create_proxy;
    use fidl_fidl_developer_bridge::DaemonMarker;
    use fidl_fuchsia_developer_remotecontrol::{
        ComponentControllerMarker, RemoteControlMarker, RemoteControlProxy, RemoteControlRequest,
    };

    fn spawn_daemon_server_with_fake_remote_control(stream: DaemonRequestStream) {
        spawn(async move {
            Daemon::new_with_proxy(setup_fake_remote_control_service())
                .handle_requests_from_stream(stream, false)
                .await
                .unwrap_or_else(|err| panic!("Fatal error handling request: {:?}", err));
        });
    }

    fn setup_fake_remote_control_service() -> RemoteControlProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<RemoteControlMarker>().unwrap();

        spawn(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(RemoteControlRequest::StartComponent { responder, .. }) => {
                        let _ = responder.send(&mut Ok(())).context("sending ok response");
                    }
                    _ => assert!(false),
                }
            }
        });

        proxy
    }

    #[test]
    fn test_echo() {
        let echo = "test-echo";
        let (daemon_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();
        hoist::run(async move {
            spawn_daemon_server_with_fake_remote_control(stream);
            let echoed = daemon_proxy.echo_string(echo).await.unwrap();
            assert_eq!(echoed, echo);
        });
    }

    #[test]
    fn test_start_component() -> Result<(), Error> {
        let url = "fuchsia-pkg://fuchsia.com/test#meta/test.cmx";
        let args = vec!["test1".to_string(), "test2".to_string()];
        let (daemon_proxy, stream) =
            fidl::endpoints::create_proxy_and_stream::<DaemonMarker>().unwrap();
        let (_, server_end) = create_proxy::<ComponentControllerMarker>()?;
        let (sout, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        let (serr, _) =
            fidl::Socket::create(fidl::SocketOpts::STREAM).context("failed to create socket")?;
        hoist::run(async move {
            spawn_daemon_server_with_fake_remote_control(stream);
            // There isn't a lot we can test here right now since this method has an empty response.
            // We just check for an Ok(()) and leave it to a real integration test to test behavior.
            daemon_proxy
                .start_component(url, &mut args.iter().map(|s| s.as_str()), sout, serr, server_end)
                .await
                .unwrap()
                .unwrap();
        });

        Ok(())
    }
}
