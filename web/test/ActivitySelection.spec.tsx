import { act, render, screen, waitFor } from "@testing-library/react";
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

it.each(["success", "error", "rejection", "return-to-first"])(
  "retires an activity-toggle %s after processor selection changes",
  async (outcome) => {
    const app = createConnectedMockZMKApp({ subsystems: ["cormoran_rip"] });
    const first = InputProcessorInfo.create({
      id: 0,
      name: "pointer_a",
      scaleMultiplier: 1,
      scaleDivisor: 1,
      inertiaThreshold: 12,
      inertiaNotificationsEnabled: false,
    });
    const second = InputProcessorInfo.create({
      ...first,
      id: 1,
      name: "scroll_b",
    });
    let encoded = Request.create({});
    jest.spyOn(Request, "encode").mockImplementation((request) => {
      encoded = request as Request;
      return { finish: () => new Uint8Array() } as ReturnType<
        typeof Request.encode
      >;
    });
    const decodeResponse = jest
      .spyOn(Response, "decode")
      .mockReturnValue(Response.create({}));
    const decodeNotification = jest.spyOn(Notification, "decode");
    let resolve!: (value: unknown) => void;
    let reject!: (error: Error) => void;
    const pending = new Promise((done, fail) => {
      resolve = done;
      reject = fail;
    });
    let requestedId: number | undefined;
    (call_rpc as jest.Mock).mockImplementation((_connection, request) => {
      if (request.core) return Promise.resolve({ core: { getLockState: 1 } });
      if (encoded.setInertiaNotifications) {
        requestedId = encoded.setInertiaNotifications.id;
        return pending;
      }
      return Promise.resolve({
        custom: { call: { payload: new Uint8Array([1]) } },
      });
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
    const notify = (processor: InputProcessorInfo) => {
      decodeNotification.mockReturnValueOnce(
        Notification.create({
          inputProcessorChanged: { processor },
        })
      );
      const subscription = (app.onNotification as jest.Mock).mock.calls
        .filter(([entry]) => entry.type === "custom")
        .at(-1)?.[0];
      expect(subscription).toBeDefined();
      act(() => subscription.callback({ payload: new Uint8Array() }));
    };
    notify(first);
    notify(second);
    const user = userEvent.setup();
    const activity = await screen.findByRole("checkbox", {
      name: "Show inertia activity",
    });
    expect(
      screen.getByRole("checkbox", { name: "Show inertia activity" })
    ).not.toBeChecked();
    await user.click(activity);
    await waitFor(() => expect(requestedId).toBe(0));
    await user.click(screen.getByText("scroll_b", { selector: "strong" }));
    if (outcome === "return-to-first") {
      await user.click(screen.getByText("pointer_a", { selector: "strong" }));
    }
    expect(
      screen.getByRole("checkbox", { name: "Show inertia activity" })
    ).not.toBeChecked();
    await act(async () => {
      if (outcome === "rejection") reject(new Error("retired activity error"));
      else {
        if (outcome === "error") {
          decodeResponse.mockReturnValueOnce(
            Response.create({
              error: { message: "retired activity error" },
            })
          );
        }
        resolve({ custom: { call: { payload: new Uint8Array([1]) } } });
      }
    });
    expect(
      screen.getByRole("checkbox", { name: "Show inertia activity" })
    ).not.toBeChecked();
    expect(
      screen.queryByText("retired activity error")
    ).not.toBeInTheDocument();
  }
);
