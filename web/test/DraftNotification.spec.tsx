import {
  act,
  fireEvent,
  render,
  screen,
  waitFor,
} from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import {
  createConnectedMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import { call_rpc } from "@zmkfirmware/zmk-studio-ts-client";
import { InputProcessorManager } from "../src/App";
import {
  InputProcessorInfo,
  Notification,
  Request,
  Response,
} from "../src/proto/cormoran/rip/custom";

jest.mock("@zmkfirmware/zmk-studio-ts-client", () => ({
  ...jest.requireActual("@zmkfirmware/zmk-studio-ts-client"),
  call_rpc: jest.fn(),
}));

afterEach(() => jest.restoreAllMocks());

it.each(["before response", "after response"])(
  "preserves edited gain and rotation when activity notifies %s",
  async (timing) => {
    const app = createConnectedMockZMKApp({ subsystems: ["cormoran_rip"] });
    const processor = InputProcessorInfo.create({
      id: 0,
      name: "pointer",
      scaleMultiplier: 1,
      scaleDivisor: 1,
      rotationDegrees: 0,
      inertiaThreshold: 12,
    });
    let encodedRequest = Request.create({});
    jest.spyOn(Request, "encode").mockImplementation((request) => {
      encodedRequest = request as Request;
      return { finish: () => new Uint8Array() } as ReturnType<
        typeof Request.encode
      >;
    });
    jest.spyOn(Response, "decode").mockReturnValue(Response.create({}));
    const decode = jest.spyOn(Notification, "decode");
    const notify = (enabled: boolean) => {
      decode.mockReturnValue(
        Notification.create({
          inputProcessorChanged: {
            processor: { ...processor, inertiaNotificationsEnabled: enabled },
          },
        })
      );
      const subscription = (app.onNotification as jest.Mock).mock.calls
        .filter(([entry]) => entry.type === "custom")
        .at(-1)?.[0];
      expect(subscription).toBeDefined();
      subscription.callback({ payload: new Uint8Array() });
    };
    const rpc = call_rpc as jest.Mock;
    rpc.mockImplementation(async (_connection, request) => {
      if (request.core) return { core: { getLockState: 1 } };
      if (
        encodedRequest.setInertiaNotifications &&
        timing === "before response"
      ) {
        notify(encodedRequest.setInertiaNotifications.enabled);
      }
      return { custom: { call: { payload: new Uint8Array([1]) } } };
    });
    render(
      <ZMKAppProvider value={app}>
        <InputProcessorManager />
      </ZMKAppProvider>
    );
    await waitFor(() =>
      expect(app.onNotification).toHaveBeenCalledWith(
        expect.objectContaining({ type: "custom" })
      )
    );
    act(() => notify(false));
    const user = userEvent.setup();
    const gain = await screen.findByRole("spinbutton", {
      name: "Scaling Multiplier:",
    });
    const rotation = screen.getByRole("spinbutton", {
      name: /Rotation.*Degrees/i,
    });
    fireEvent.change(gain, { target: { value: "7" } });
    await user.clear(rotation);
    await user.type(rotation, "15");
    await user.click(
      screen.getByRole("checkbox", { name: "Show inertia activity" })
    );
    await waitFor(() =>
      expect(
        screen.getByRole("checkbox", { name: "Show inertia activity" })
      ).toBeChecked()
    );
    if (timing === "after response") act(() => notify(true));
    expect(gain).toHaveValue(7);
    expect(rotation).toHaveValue(15);
    expect(
      screen.getByRole("checkbox", { name: "Show inertia activity" })
    ).toBeChecked();
  }
);
